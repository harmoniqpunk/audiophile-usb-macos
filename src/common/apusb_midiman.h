/*
 * apusb_midiman.h - framing used by the Audiophile USB MIDI interface.
 *
 * Every 4-byte USB packet holds up to three raw MIDI bytes in bytes 0..2
 * and a tag in byte 3: high nibble = port (cable) number, low two bits =
 * number of valid MIDI bytes (1..3). A tag of 0 marks an unused slot.
 * (See docs/PROTOCOL.md.)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_MIDIMAN_H
#define APUSB_MIDIMAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*apusb_midiman_sink)(void *ctx, unsigned port, const uint8_t *bytes, unsigned len);

/* Decode an incoming buffer; calls sink once per valid packet. */
void apusb_midiman_decode(const uint8_t *buf, size_t len, apusb_midiman_sink sink, void *ctx);

/*
 * Stateful encoder: splits a raw MIDI byte stream into packets so that
 * each packet carries one complete short message (or a 1..3 byte SysEx
 * chunk). Real-time bytes are sent in their own packet immediately.
 */
typedef struct {
    uint8_t  pending[3];
    unsigned count;
    unsigned expected;      /* bytes needed to complete current message */
    uint8_t  runningStatus; /* 0 = none                                  */
    int      inSysex;
    unsigned port;
} apusb_midiman_encoder;

void   apusb_midiman_encoder_init(apusb_midiman_encoder *e, unsigned port);
/* Returns bytes written to out (multiple of 4). outCap must be >= 4 * (len + 1). */
size_t apusb_midiman_encode(apusb_midiman_encoder *e, const uint8_t *midi, size_t len,
                            uint8_t *out, size_t outCap);
/* Flush a partially collected SysEx chunk. */
size_t apusb_midiman_flush(apusb_midiman_encoder *e, uint8_t *out, size_t outCap);

#ifdef __cplusplus
}
#endif
#endif
