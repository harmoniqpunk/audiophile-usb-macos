/*
 * apusb_modes.h - operating modes of the M-Audio Audiophile USB (0763:2003).
 *
 * Clean-room description built from publicly documented behaviour
 * (see docs/PROTOCOL.md for sources). No third-party code is used.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_MODES_H
#define APUSB_MODES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APUSB_VENDOR_ID      0x0763u
#define APUSB_PRODUCT_ID     0x2003u /* normal operation               */
#define APUSB_PRODUCT_ID_DFU 0x2803u /* firmware-update (DFU) identity */

/* USB interface numbers (documented layout). */
enum {
    APUSB_IF_VENDOR      = 0,
    APUSB_IF_CONTROL     = 1, /* USB Audio Class 1.0 AudioControl */
    APUSB_IF_ANALOG_OUT  = 2,
    APUSB_IF_DIGITAL_OUT = 3,
    APUSB_IF_ANALOG_IN   = 4,
    APUSB_IF_DIGITAL_IN  = 5,
    APUSB_IF_MIDI        = 6  /* vendor-specific "Midiman" MIDI    */
};

/* First and last audio streaming interface. */
#define APUSB_IF_STREAM_FIRST APUSB_IF_ANALOG_OUT
#define APUSB_IF_STREAM_LAST  APUSB_IF_DIGITAL_IN

typedef enum {
    APUSB_FMT_S16LE = 0,
    APUSB_FMT_S16BE,
    APUSB_FMT_S24_3LE,
    APUSB_FMT_S24_3BE
} apusb_format;

#define APUSB_MAX_RATES 4
#define APUSB_CHANNELS  2

typedef struct {
    const char  *name;        /* stable identifier, e.g. "24bit-48k"  */
    const char  *description; /* human readable                       */
    int          outInterface;/* streaming interface for playback, -1 = none */
    int          inInterface; /* streaming interface for capture,  -1 = none */
    uint8_t      altSetting;  /* alternate setting used on both interfaces    */
    apusb_format format;      /* on-the-wire sample format                    */
    unsigned     numRates;
    double       rates[APUSB_MAX_RATES];
    double       defaultRate;
} apusb_mode;

unsigned          apusb_mode_count(void);
const apusb_mode *apusb_mode_at(unsigned index);
const apusb_mode *apusb_mode_by_name(const char *name); /* NULL if unknown */
const apusb_mode *apusb_mode_default(void);
bool              apusb_mode_supports_rate(const apusb_mode *mode, double rate);

unsigned     apusb_format_bytes(apusb_format fmt);           /* bytes per sample */
const char  *apusb_format_name(apusb_format fmt);
bool         apusb_format_parse(const char *s, apusb_format *out);

#ifdef __cplusplus
}
#endif
#endif
