/*
 * apusb_ring.c
 * SPDX-License-Identifier: MIT
 */
#include "apusb_ring.h"

#include <stdlib.h>
#include <string.h>

bool apusb_ring_init(apusb_ring *r, uint32_t frames, uint32_t channels)
{
    if (frames == 0 || (frames & (frames - 1)) != 0 || channels == 0) return false;
    r->data = (float *)calloc((size_t)frames * channels, sizeof(float));
    r->frames = frames;
    r->channels = channels;
    return r->data != NULL;
}

void apusb_ring_free(apusb_ring *r)
{
    free(r->data);
    r->data = NULL;
    r->frames = 0;
}

void apusb_ring_clear(apusb_ring *r)
{
    if (r->data) memset(r->data, 0, (size_t)r->frames * r->channels * sizeof(float));
}

static inline uint32_t ring_index(const apusb_ring *r, int64_t t)
{
    return (uint32_t)((uint64_t)t & (uint64_t)(r->frames - 1));
}

void apusb_ring_write(apusb_ring *r, int64_t sampleTime, const float *src, uint32_t frames)
{
    if (r->data == NULL) return;
    while (frames > 0) {
        uint32_t idx = ring_index(r, sampleTime);
        uint32_t chunk = r->frames - idx;
        if (chunk > frames) chunk = frames;
        memcpy(r->data + (size_t)idx * r->channels, src, (size_t)chunk * r->channels * sizeof(float));
        src += (size_t)chunk * r->channels;
        sampleTime += chunk;
        frames -= chunk;
    }
}

void apusb_ring_read(apusb_ring *r, int64_t sampleTime, float *dst, uint32_t frames, bool clearAfterRead)
{
    if (r->data == NULL) { memset(dst, 0, (size_t)frames * r->channels * sizeof(float)); return; }
    while (frames > 0) {
        uint32_t idx = ring_index(r, sampleTime);
        uint32_t chunk = r->frames - idx;
        if (chunk > frames) chunk = frames;
        float *p = r->data + (size_t)idx * r->channels;
        memcpy(dst, p, (size_t)chunk * r->channels * sizeof(float));
        if (clearAfterRead) memset(p, 0, (size_t)chunk * r->channels * sizeof(float));
        dst += (size_t)chunk * r->channels;
        sampleTime += chunk;
        frames -= chunk;
    }
}
