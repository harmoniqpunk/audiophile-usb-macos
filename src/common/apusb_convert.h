/*
 * apusb_convert.h - Float32 <-> device PCM conversion and USB packet pacing.
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_CONVERT_H
#define APUSB_CONVERT_H

#include <stddef.h>
#include <stdint.h>
#include "apusb_modes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interleaved float [-1,1] -> packed device samples (clipped). */
void apusb_float_to_pcm(const float *src, uint8_t *dst, size_t frames,
                        unsigned channels, apusb_format fmt);
/* Packed device samples -> interleaved float. */
void apusb_pcm_to_float(const uint8_t *src, float *dst, size_t frames,
                        unsigned channels, apusb_format fmt);

/*
 * Full-speed USB carries one isochronous packet per 1 ms frame. The pacer
 * spreads a non-integer frames-per-ms rate (e.g. 44.1) over packets and
 * applies a +/-1 sample correction so an adaptive endpoint follows the host
 * clock.
 */
typedef struct {
    uint32_t rate;      /* samples per second */
    uint32_t remainder; /* accumulated sub-millisecond remainder, 0..999 */
} apusb_pacer;

void     apusb_pacer_init(apusb_pacer *p, uint32_t rate);
/* correction: <0 one fewer sample, >0 one more, 0 nominal. */
unsigned apusb_pacer_next(apusb_pacer *p, int correction);
/* Largest packet (in sample frames) the pacer can ever return. */
unsigned apusb_pacer_max_frames(uint32_t rate);

#ifdef __cplusplus
}
#endif
#endif
