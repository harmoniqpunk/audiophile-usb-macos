/*
 * apusb_stream.h - isochronous streaming engine (one direction, one pipe).
 *
 * Runs its own real-time (time-constraint) thread with a CFRunLoop that
 * services IOUSBLib completions. Keeps `xfers` transfers of `framesPerXfer`
 * USB frames (1 ms each at full speed) in flight. The IO path takes no
 * locks and performs no allocations.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_STREAM_H
#define APUSB_STREAM_H

#include <stdbool.h>
#include <stdint.h>
#include "apusb_usb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APUSB_STREAM_MAX_XFERS           16
#define APUSB_STREAM_MAX_FRAMES_PER_XFER 16
/* Low-latency defaults: 4 transfers x 2 ms = 8 ms of playback queued. */
#define APUSB_STREAM_DEFAULT_XFERS           4
#define APUSB_STREAM_DEFAULT_FRAMES_PER_XFER 2

typedef struct apusb_stream apusb_stream;

typedef struct {
    /* Playback: choose how many sample frames the next 1 ms packet carries. */
    unsigned (*packet_frames)(void *ctx);
    /* Playback: fill `frames` sample frames of device PCM into dst. */
    void (*fill)(void *ctx, uint8_t *dst, unsigned frames);
    /* Capture: consume `frames` sample frames of device PCM. */
    void (*consume)(void *ctx, const uint8_t *src, unsigned frames);
    /* Optional: error / status notification (called on stream thread). */
    void (*status)(void *ctx, IOReturn result, const char *what);
    void *ctx;
} apusb_stream_callbacks;

apusb_stream *apusb_stream_create(apusb_interface *intf, const apusb_pipe *pipe, bool isInput,
                                  unsigned bytesPerFrame, unsigned xfers, unsigned framesPerXfer,
                                  const apusb_stream_callbacks *cb);
IOReturn      apusb_stream_start(apusb_stream *s);
void          apusb_stream_stop(apusb_stream *s);
void          apusb_stream_destroy(apusb_stream *s);
/* Counters for diagnostics. */
void          apusb_stream_stats(apusb_stream *s, uint64_t *packets, uint64_t *errors, uint64_t *resyncs);

#ifdef __cplusplus
}
#endif
#endif
