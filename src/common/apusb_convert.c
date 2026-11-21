/*
 * apusb_convert.c
 * SPDX-License-Identifier: MIT
 */
#include "apusb_convert.h"

#include <math.h>

static inline int32_t clamp_scale(float v, float scale, int32_t lo, int32_t hi)
{
    if (!(v == v)) v = 0.0f; /* NaN -> silence */
    double s = (double)v * (double)scale;
    s = s < 0 ? s - 0.5 : s + 0.5;   /* round half away from zero */
    if (s < (double)lo) return lo;
    if (s > (double)hi) return hi;
    return (int32_t)s;
}

void apusb_float_to_pcm(const float *src, uint8_t *dst, size_t frames,
                        unsigned channels, apusb_format fmt)
{
    size_t n = frames * channels;
    switch (fmt) {
    case APUSB_FMT_S16LE:
    case APUSB_FMT_S16BE:
        for (size_t i = 0; i < n; i++) {
            int32_t s = clamp_scale(src[i], 32768.0f, -32768, 32767);
            uint8_t lo = (uint8_t)(s & 0xff), hi = (uint8_t)((s >> 8) & 0xff);
            if (fmt == APUSB_FMT_S16LE) { dst[0] = lo; dst[1] = hi; }
            else                        { dst[0] = hi; dst[1] = lo; }
            dst += 2;
        }
        break;
    case APUSB_FMT_S24_3LE:
    case APUSB_FMT_S24_3BE:
        for (size_t i = 0; i < n; i++) {
            int32_t s = clamp_scale(src[i], 8388608.0f, -8388608, 8388607);
            uint8_t b0 = (uint8_t)(s & 0xff), b1 = (uint8_t)((s >> 8) & 0xff),
                    b2 = (uint8_t)((s >> 16) & 0xff);
            if (fmt == APUSB_FMT_S24_3LE) { dst[0] = b0; dst[1] = b1; dst[2] = b2; }
            else                          { dst[0] = b2; dst[1] = b1; dst[2] = b0; }
            dst += 3;
        }
        break;
    }
}

void apusb_pcm_to_float(const uint8_t *src, float *dst, size_t frames,
                        unsigned channels, apusb_format fmt)
{
    size_t n = frames * channels;
    switch (fmt) {
    case APUSB_FMT_S16LE:
    case APUSB_FMT_S16BE:
        for (size_t i = 0; i < n; i++) {
            uint16_t u = (fmt == APUSB_FMT_S16LE)
                ? (uint16_t)(src[0] | (src[1] << 8))
                : (uint16_t)((src[0] << 8) | src[1]);
            dst[i] = (float)(int16_t)u / 32768.0f;
            src += 2;
        }
        break;
    case APUSB_FMT_S24_3LE:
    case APUSB_FMT_S24_3BE: {
        for (size_t i = 0; i < n; i++) {
            uint32_t u = (fmt == APUSB_FMT_S24_3LE)
                ? ((uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16))
                : ((uint32_t)src[2] | ((uint32_t)src[1] << 8) | ((uint32_t)src[0] << 16));
            int32_t s = (int32_t)(u << 8) >> 8; /* sign-extend 24 -> 32 */
            dst[i] = (float)s / 8388608.0f;
            src += 3;
        }
        break;
    }
    }
}

void apusb_pacer_init(apusb_pacer *p, uint32_t rate)
{
    p->rate = rate;
    p->remainder = 0;
}

unsigned apusb_pacer_next(apusb_pacer *p, int correction)
{
    unsigned n = p->rate / 1000u;
    p->remainder += p->rate % 1000u;
    if (p->remainder >= 1000u) { p->remainder -= 1000u; n++; }
    if (correction > 0) n++;
    else if (correction < 0 && n > 0) n--;
    return n;
}

unsigned apusb_pacer_max_frames(uint32_t rate)
{
    return rate / 1000u + 2u; /* nominal + remainder carry + correction */
}
