/*
 * apusb_midiman.c
 * SPDX-License-Identifier: MIT
 */
#include "apusb_midiman.h"

void apusb_midiman_decode(const uint8_t *buf, size_t len, apusb_midiman_sink sink, void *ctx)
{
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint8_t tag = buf[i + 3];
        if (tag == 0) continue;
        unsigned n = tag & 0x03u;
        if (n == 0) continue;
        sink(ctx, tag >> 4, &buf[i], n);
    }
}

static unsigned message_length(uint8_t status)
{
    if (status < 0xf0) {
        switch (status & 0xf0) {
        case 0xc0: case 0xd0: return 2;
        default: return 3;
        }
    }
    switch (status) {
    case 0xf1: case 0xf3: return 2;
    case 0xf2: return 3;
    default: return 1; /* f6, f7, undefined */
    }
}

void apusb_midiman_encoder_init(apusb_midiman_encoder *e, unsigned port)
{
    e->count = 0;
    e->expected = 0;
    e->runningStatus = 0;
    e->inSysex = 0;
    e->port = port & 0x0fu;
}

static size_t emit(apusb_midiman_encoder *e, const uint8_t *b, unsigned n, uint8_t *out, size_t cap)
{
    if (n == 0 || cap < 4) return 0;
    out[0] = n > 0 ? b[0] : 0;
    out[1] = n > 1 ? b[1] : 0;
    out[2] = n > 2 ? b[2] : 0;
    out[3] = (uint8_t)((e->port << 4) | (n & 0x03u));
    return 4;
}

size_t apusb_midiman_flush(apusb_midiman_encoder *e, uint8_t *out, size_t outCap)
{
    size_t w = 0;
    if (e->inSysex && e->count > 0) {
        w = emit(e, e->pending, e->count, out, outCap);
        e->count = 0;
    }
    return w;
}

size_t apusb_midiman_encode(apusb_midiman_encoder *e, const uint8_t *midi, size_t len,
                            uint8_t *out, size_t outCap)
{
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = midi[i];
        if (outCap - w < 8) break; /* keep room for a flush + this byte */

        if (b >= 0xf8) { /* real-time: never interrupts state */
            w += emit(e, &b, 1, out + w, outCap - w);
            continue;
        }
        if (b == 0xf0) { /* SysEx start */
            e->inSysex = 1;
            e->runningStatus = 0;
            e->count = 0;
            e->pending[e->count++] = b;
            continue;
        }
        if (e->inSysex) {
            if (b & 0x80) {
                /* F7 or any status ends SysEx */
                if (b == 0xf7) {
                    e->pending[e->count++] = b;
                    w += emit(e, e->pending, e->count, out + w, outCap - w);
                    e->count = 0;
                    e->inSysex = 0;
                    continue;
                }
                w += apusb_midiman_flush(e, out + w, outCap - w);
                e->inSysex = 0;
                /* fall through to status handling below */
            } else {
                e->pending[e->count++] = b;
                if (e->count == 3) {
                    w += emit(e, e->pending, 3, out + w, outCap - w);
                    e->count = 0;
                }
                continue;
            }
        }
        if (b & 0x80) { /* status byte */
            e->count = 0;
            e->pending[e->count++] = b;
            e->expected = message_length(b);
            e->runningStatus = (b < 0xf0) ? b : 0;
            if (e->expected == 1) {
                w += emit(e, e->pending, 1, out + w, outCap - w);
                e->count = 0;
            }
            continue;
        }
        /* data byte */
        if (e->count == 0) {
            if (e->runningStatus == 0) continue; /* stray data byte: drop */
            /* Running status: re-insert the status so every packet is a
             * complete message (the device is not assumed to track it). */
            e->pending[e->count++] = e->runningStatus;
            e->expected = message_length(e->runningStatus);
        }
        e->pending[e->count++] = b;
        if (e->count >= e->expected) {
            w += emit(e, e->pending, e->count, out + w, outCap - w);
            e->count = 0;
        }
    }
    return w;
}
