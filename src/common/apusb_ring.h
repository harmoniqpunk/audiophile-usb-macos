/*
 * apusb_ring.h - interleaved float ring buffer addressed by absolute sample
 * time (single producer / single consumer, lock-free by construction).
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_RING_H
#define APUSB_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float   *data;
    uint32_t frames;   /* power of two */
    uint32_t channels;
} apusb_ring;

bool apusb_ring_init(apusb_ring *r, uint32_t frames, uint32_t channels);
void apusb_ring_free(apusb_ring *r);
void apusb_ring_clear(apusb_ring *r);
/* Copy `frames` frames to/from absolute position `sampleTime` (may be negative). */
void apusb_ring_write(apusb_ring *r, int64_t sampleTime, const float *src, uint32_t frames);
void apusb_ring_read(apusb_ring *r, int64_t sampleTime, float *dst, uint32_t frames, bool clearAfterRead);

#ifdef __cplusplus
}
#endif
#endif
