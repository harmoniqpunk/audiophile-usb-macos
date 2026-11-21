/*
 * apusb_stream.c - isochronous streaming engine.
 * SPDX-License-Identifier: MIT
 */
#include "apusb_stream.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    apusb_stream   *owner;
    uint8_t        *buffer;
    IOUSBIsocFrame  frames[APUSB_STREAM_FRAMES_PER_XFER];
    bool            inFlight;
} xfer_t;

struct apusb_stream {
    apusb_interface       *intf;
    apusb_pipe             pipe;
    bool                   isInput;
    unsigned               bytesPerFrame;
    apusb_stream_callbacks cb;

    xfer_t                 xfers[APUSB_STREAM_XFERS];
    UInt64                 nextFrame;

    pthread_t              thread;
    CFRunLoopRef           runLoop;
    CFRunLoopSourceRef     source;
    pthread_mutex_t        lock;
    pthread_cond_t         cond;
    bool                   threadReady;
    bool                   running;
    bool                   stopping;
    int                    inFlight;
    IOReturn               startResult;

    uint64_t               packets, errors, resyncs;
};

static void report(apusb_stream *s, IOReturn r, const char *what)
{
    if (s->cb.status) s->cb.status(s->cb.ctx, r, what);
}

static IOReturn resync_frame(apusb_stream *s)
{
    UInt64 frame = 0;
    AbsoluteTime at;
    IOReturn kr = (*s->intf->intf)->GetBusFrameNumber(s->intf->intf, &frame, &at);
    if (kr == kIOReturnSuccess) s->nextFrame = frame + 4; /* small scheduling margin */
    return kr;
}

static void completion(void *refcon, IOReturn result, void *arg0);

static IOReturn submit(apusb_stream *s, xfer_t *x)
{
    unsigned mps = s->pipe.maxPacketSize;
    for (int f = 0; f < APUSB_STREAM_FRAMES_PER_XFER; f++) {
        x->frames[f].frStatus = 0;
        x->frames[f].frActCount = 0;
        if (s->isInput) {
            x->frames[f].frReqCount = (UInt16)mps;
        } else {
            unsigned n = s->cb.packet_frames(s->cb.ctx);
            unsigned bytes = n * s->bytesPerFrame;
            if (bytes > mps) { n = mps / s->bytesPerFrame; bytes = n * s->bytesPerFrame; }
            s->cb.fill(s->cb.ctx, x->buffer + (size_t)f * mps, n);
            x->frames[f].frReqCount = (UInt16)bytes;
        }
    }
    /* Output data must be contiguous: compact the per-frame slots. */
    if (!s->isInput) {
        size_t off = 0;
        for (int f = 0; f < APUSB_STREAM_FRAMES_PER_XFER; f++) {
            size_t len = x->frames[f].frReqCount;
            if (off != (size_t)f * mps) memmove(x->buffer + off, x->buffer + (size_t)f * mps, len);
            off += len;
        }
    }

    IOReturn kr = kIOReturnError;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s->isInput)
            kr = (*s->intf->intf)->ReadIsochPipeAsync(s->intf->intf, s->pipe.pipeRef, x->buffer,
                                                      s->nextFrame, APUSB_STREAM_FRAMES_PER_XFER,
                                                      x->frames, completion, x);
        else
            kr = (*s->intf->intf)->WriteIsochPipeAsync(s->intf->intf, s->pipe.pipeRef, x->buffer,
                                                       s->nextFrame, APUSB_STREAM_FRAMES_PER_XFER,
                                                       x->frames, completion, x);
        if (kr == kIOReturnIsoTooOld || kr == kIOReturnIsoTooNew) {
            s->resyncs++;
            resync_frame(s);
            continue;
        }
        break;
    }
    if (kr == kIOReturnSuccess) {
        x->inFlight = true;
        s->inFlight++;
        s->nextFrame += APUSB_STREAM_FRAMES_PER_XFER;
    } else {
        s->errors++;
        report(s, kr, s->isInput ? "ReadIsochPipeAsync" : "WriteIsochPipeAsync");
    }
    return kr;
}

static void completion(void *refcon, IOReturn result, void *arg0)
{
    (void)arg0;
    xfer_t *x = (xfer_t *)refcon;
    apusb_stream *s = x->owner;

    pthread_mutex_lock(&s->lock);
    x->inFlight = false;
    s->inFlight--;
    bool stopping = s->stopping;
    pthread_mutex_unlock(&s->lock);

    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        if (result != kIOReturnAborted) { s->errors++; report(s, result, "isoch completion"); }
    }

    if (s->isInput && result != kIOReturnAborted) {
        unsigned mps = s->pipe.maxPacketSize;
        /* Input frames land at their per-frame offset (frReqCount = mps). */
        for (int f = 0; f < APUSB_STREAM_FRAMES_PER_XFER; f++) {
            unsigned got = x->frames[f].frActCount;
            if (got == 0) continue;
            s->cb.consume(s->cb.ctx, x->buffer + (size_t)f * mps, got / s->bytesPerFrame);
        }
    }
    s->packets += APUSB_STREAM_FRAMES_PER_XFER;

    if (stopping) {
        pthread_mutex_lock(&s->lock);
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        return;
    }
    pthread_mutex_lock(&s->lock);
    submit(s, x);
    pthread_mutex_unlock(&s->lock);
}

static void *thread_main(void *arg)
{
    apusb_stream *s = (apusb_stream *)arg;
    pthread_setname_np(s->isInput ? "apusb.capture" : "apusb.playback");

    pthread_mutex_lock(&s->lock);
    s->runLoop = CFRunLoopGetCurrent();
    CFRetain(s->runLoop);
    CFRunLoopAddSource(s->runLoop, s->source, kCFRunLoopDefaultMode);

    IOReturn kr = resync_frame(s);
    s->nextFrame += 8; /* first transfer a little later */
    for (int i = 0; i < APUSB_STREAM_XFERS && kr == kIOReturnSuccess; i++)
        kr = submit(s, &s->xfers[i]);
    s->startResult = kr;
    s->threadReady = true;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    if (kr == kIOReturnSuccess) CFRunLoopRun();

    CFRunLoopRemoveSource(s->runLoop, s->source, kCFRunLoopDefaultMode);
    return NULL;
}

apusb_stream *apusb_stream_create(apusb_interface *intf, const apusb_pipe *pipe, bool isInput,
                                  unsigned bytesPerFrame, const apusb_stream_callbacks *cb)
{
    if (intf == NULL || pipe == NULL || cb == NULL || bytesPerFrame == 0) return NULL;
    if (isInput ? cb->consume == NULL : (cb->fill == NULL || cb->packet_frames == NULL)) return NULL;

    apusb_stream *s = (apusb_stream *)calloc(1, sizeof *s);
    if (s == NULL) return NULL;
    s->intf = intf;
    s->pipe = *pipe;
    s->isInput = isInput;
    s->bytesPerFrame = bytesPerFrame;
    s->cb = *cb;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    for (int i = 0; i < APUSB_STREAM_XFERS; i++) {
        s->xfers[i].owner = s;
        s->xfers[i].buffer = (uint8_t *)calloc(APUSB_STREAM_FRAMES_PER_XFER, pipe->maxPacketSize);
        if (s->xfers[i].buffer == NULL) { apusb_stream_destroy(s); return NULL; }
    }
    return s;
}

IOReturn apusb_stream_start(apusb_stream *s)
{
    if (s->running) return kIOReturnSuccess;
    IOReturn kr = (*s->intf->intf)->CreateInterfaceAsyncEventSource(s->intf->intf, &s->source);
    if (kr != kIOReturnSuccess) return kr;

    s->stopping = false;
    s->threadReady = false;
    if (pthread_create(&s->thread, NULL, thread_main, s) != 0) return kIOReturnNoResources;

    pthread_mutex_lock(&s->lock);
    while (!s->threadReady) pthread_cond_wait(&s->cond, &s->lock);
    kr = s->startResult;
    pthread_mutex_unlock(&s->lock);

    s->running = true;
    if (kr != kIOReturnSuccess) apusb_stream_stop(s);
    return kr;
}

void apusb_stream_stop(apusb_stream *s)
{
    if (!s->running) return;
    pthread_mutex_lock(&s->lock);
    s->stopping = true;
    pthread_mutex_unlock(&s->lock);

    (*s->intf->intf)->AbortPipe(s->intf->intf, s->pipe.pipeRef);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    pthread_mutex_lock(&s->lock);
    while (s->inFlight > 0) {
        if (pthread_cond_timedwait(&s->cond, &s->lock, &deadline) != 0) break;
    }
    pthread_mutex_unlock(&s->lock);

    if (s->runLoop) CFRunLoopStop(s->runLoop);
    pthread_join(s->thread, NULL);
    if (s->runLoop) { CFRelease(s->runLoop); s->runLoop = NULL; }
    if (s->source) { CFRelease(s->source); s->source = NULL; }
    s->running = false;
}

void apusb_stream_destroy(apusb_stream *s)
{
    if (s == NULL) return;
    apusb_stream_stop(s);
    for (int i = 0; i < APUSB_STREAM_XFERS; i++) free(s->xfers[i].buffer);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}

void apusb_stream_stats(apusb_stream *s, uint64_t *packets, uint64_t *errors, uint64_t *resyncs)
{
    if (packets) *packets = s->packets;
    if (errors) *errors = s->errors;
    if (resyncs) *resyncs = s->resyncs;
}
