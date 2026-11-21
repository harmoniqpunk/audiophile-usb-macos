/*
 * apusb_stream.c - isochronous streaming engine.
 * SPDX-License-Identifier: MIT
 */
#include "apusb_stream.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    apusb_stream   *owner;
    uint8_t        *buffer;
    IOUSBIsocFrame  frames[APUSB_STREAM_MAX_FRAMES_PER_XFER];
} xfer_t;

struct apusb_stream {
    apusb_interface       *intf;
    apusb_pipe             pipe;
    bool                   isInput;
    unsigned               bytesPerFrame;
    unsigned               numXfers;
    unsigned               framesPerXfer;
    apusb_stream_callbacks cb;

    xfer_t                 xfers[APUSB_STREAM_MAX_XFERS];
    UInt64                 nextFrame;   /* touched only on the stream thread */

    pthread_t              thread;
    CFRunLoopRef           runLoop;
    CFRunLoopSourceRef     source;
    pthread_mutex_t        startLock;   /* start/stop only, never in the IO path */
    pthread_cond_t         startCond;
    bool                   threadReady;
    bool                   running;
    _Atomic bool           stopping;
    _Atomic int            inFlight;
    IOReturn               startResult;

    _Atomic uint64_t       packets, errors, resyncs;
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
    if (kr == kIOReturnSuccess) s->nextFrame = frame + 2; /* minimal scheduling margin */
    return kr;
}

static void completion(void *refcon, IOReturn result, void *arg0);

static IOReturn submit(apusb_stream *s, xfer_t *x)
{
    unsigned mps = s->pipe.maxPacketSize;
    unsigned nf = s->framesPerXfer;
    size_t off = 0;
    for (unsigned f = 0; f < nf; f++) {
        x->frames[f].frStatus = 0;
        x->frames[f].frActCount = 0;
        if (s->isInput) {
            x->frames[f].frReqCount = (UInt16)mps;
        } else {
            unsigned n = s->cb.packet_frames(s->cb.ctx);
            unsigned bytes = n * s->bytesPerFrame;
            if (bytes > mps) { n = mps / s->bytesPerFrame; bytes = n * s->bytesPerFrame; }
            s->cb.fill(s->cb.ctx, x->buffer + off, n); /* output data is contiguous */
            x->frames[f].frReqCount = (UInt16)bytes;
            off += bytes;
        }
    }

    IOReturn kr = kIOReturnError;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (s->isInput)
            kr = (*s->intf->intf)->ReadIsochPipeAsync(s->intf->intf, s->pipe.pipeRef, x->buffer,
                                                      s->nextFrame, nf, x->frames, completion, x);
        else
            kr = (*s->intf->intf)->WriteIsochPipeAsync(s->intf->intf, s->pipe.pipeRef, x->buffer,
                                                       s->nextFrame, nf, x->frames, completion, x);
        if (kr == kIOReturnIsoTooOld || kr == kIOReturnIsoTooNew) {
            atomic_fetch_add(&s->resyncs, 1);
            resync_frame(s);
            continue;
        }
        break;
    }
    if (kr == kIOReturnSuccess) {
        atomic_fetch_add(&s->inFlight, 1);
        s->nextFrame += nf;
    } else {
        atomic_fetch_add(&s->errors, 1);
        report(s, kr, s->isInput ? "ReadIsochPipeAsync" : "WriteIsochPipeAsync");
    }
    return kr;
}

static void completion(void *refcon, IOReturn result, void *arg0)
{
    (void)arg0;
    xfer_t *x = (xfer_t *)refcon;
    apusb_stream *s = x->owner;
    atomic_fetch_sub(&s->inFlight, 1);

    if (result != kIOReturnSuccess && result != kIOReturnUnderrun && result != kIOReturnAborted) {
        atomic_fetch_add(&s->errors, 1);
        report(s, result, "isoch completion");
    }
    if (s->isInput && result != kIOReturnAborted) {
        unsigned mps = s->pipe.maxPacketSize;
        /* Input frames land at their per-frame offset (frReqCount = mps). */
        for (unsigned f = 0; f < s->framesPerXfer; f++) {
            unsigned got = x->frames[f].frActCount;
            if (got) s->cb.consume(s->cb.ctx, x->buffer + (size_t)f * mps, got / s->bytesPerFrame);
        }
    }
    atomic_fetch_add(&s->packets, s->framesPerXfer);
    if (atomic_load(&s->stopping) || result == kIOReturnAborted || result == kIOReturnNoDevice) return;
    submit(s, x);
}

static void make_realtime(unsigned periodMs)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticksPerMs = 1.0e6 * (double)tb.denom / (double)tb.numer;
    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)(ticksPerMs * periodMs);
    p.computation = (uint32_t)(ticksPerMs * 0.25);
    p.constraint = (uint32_t)(ticksPerMs * periodMs);
    p.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static void *thread_main(void *arg)
{
    apusb_stream *s = (apusb_stream *)arg;
    pthread_setname_np(s->isInput ? "apusb.capture" : "apusb.playback");
    make_realtime(s->framesPerXfer);

    pthread_mutex_lock(&s->startLock);
    s->runLoop = CFRunLoopGetCurrent();
    CFRetain(s->runLoop);
    CFRunLoopAddSource(s->runLoop, s->source, kCFRunLoopDefaultMode);

    IOReturn kr = resync_frame(s);
    for (unsigned i = 0; i < s->numXfers && kr == kIOReturnSuccess; i++)
        kr = submit(s, &s->xfers[i]);
    s->startResult = kr;
    s->threadReady = true;
    pthread_cond_broadcast(&s->startCond);
    pthread_mutex_unlock(&s->startLock);

    if (kr == kIOReturnSuccess) CFRunLoopRun();

    CFRunLoopRemoveSource(s->runLoop, s->source, kCFRunLoopDefaultMode);
    return NULL;
}

apusb_stream *apusb_stream_create(apusb_interface *intf, const apusb_pipe *pipe, bool isInput,
                                  unsigned bytesPerFrame, unsigned xfers, unsigned framesPerXfer,
                                  const apusb_stream_callbacks *cb)
{
    if (intf == NULL || pipe == NULL || cb == NULL || bytesPerFrame == 0) return NULL;
    if (isInput ? cb->consume == NULL : (cb->fill == NULL || cb->packet_frames == NULL)) return NULL;
    if (xfers < 2) xfers = 2;
    if (xfers > APUSB_STREAM_MAX_XFERS) xfers = APUSB_STREAM_MAX_XFERS;
    if (framesPerXfer < 1) framesPerXfer = 1;
    if (framesPerXfer > APUSB_STREAM_MAX_FRAMES_PER_XFER) framesPerXfer = APUSB_STREAM_MAX_FRAMES_PER_XFER;

    apusb_stream *s = (apusb_stream *)calloc(1, sizeof *s);
    if (s == NULL) return NULL;
    s->intf = intf;
    s->pipe = *pipe;
    s->isInput = isInput;
    s->bytesPerFrame = bytesPerFrame;
    s->numXfers = xfers;
    s->framesPerXfer = framesPerXfer;
    s->cb = *cb;
    pthread_mutex_init(&s->startLock, NULL);
    pthread_cond_init(&s->startCond, NULL);
    for (unsigned i = 0; i < xfers; i++) {
        s->xfers[i].owner = s;
        s->xfers[i].buffer = (uint8_t *)calloc(framesPerXfer, pipe->maxPacketSize);
        if (s->xfers[i].buffer == NULL) { apusb_stream_destroy(s); return NULL; }
    }
    return s;
}

IOReturn apusb_stream_start(apusb_stream *s)
{
    if (s->running) return kIOReturnSuccess;
    IOReturn kr = (*s->intf->intf)->CreateInterfaceAsyncEventSource(s->intf->intf, &s->source);
    if (kr != kIOReturnSuccess) return kr;

    atomic_store(&s->stopping, false);
    s->threadReady = false;
    if (pthread_create(&s->thread, NULL, thread_main, s) != 0) return kIOReturnNoResources;

    pthread_mutex_lock(&s->startLock);
    while (!s->threadReady) pthread_cond_wait(&s->startCond, &s->startLock);
    kr = s->startResult;
    pthread_mutex_unlock(&s->startLock);

    s->running = true;
    if (kr != kIOReturnSuccess) apusb_stream_stop(s);
    return kr;
}

void apusb_stream_stop(apusb_stream *s)
{
    if (!s->running) return;
    atomic_store(&s->stopping, true);
    (*s->intf->intf)->AbortPipe(s->intf->intf, s->pipe.pipeRef);
    for (int i = 0; i < 200 && atomic_load(&s->inFlight) > 0; i++) usleep(5000); /* <= 1 s */
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
    for (unsigned i = 0; i < APUSB_STREAM_MAX_XFERS; i++) free(s->xfers[i].buffer);
    pthread_mutex_destroy(&s->startLock);
    pthread_cond_destroy(&s->startCond);
    free(s);
}

void apusb_stream_stats(apusb_stream *s, uint64_t *packets, uint64_t *errors, uint64_t *resyncs)
{
    if (packets) *packets = atomic_load(&s->packets);
    if (errors) *errors = atomic_load(&s->errors);
    if (resyncs) *resyncs = atomic_load(&s->resyncs);
}
