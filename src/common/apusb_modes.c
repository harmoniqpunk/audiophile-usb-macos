/*
 * apusb_modes.c - mode table for the M-Audio Audiophile USB.
 *
 * Alternate-setting meanings (documented, see docs/PROTOCOL.md):
 *   alt 1 : 24-bit, 48.1-96 kHz, only ONE port at a time (half duplex)
 *   alt 2 : 24-bit, <=48 kHz, digital input enabled
 *   alt 3 : 24-bit, <=48 kHz, digital input disabled (not on interface 5)
 *   alt 4 : 16-bit, <=48 kHz, digital input enabled
 *   alt 5 : 16-bit, <=48 kHz, digital input disabled (not on interface 5)
 *   alt 6 : 16-bit, AC-3/DTS pass-through (digital out only) - not exposed
 * 24-bit data is big-endian on the wire; 16-bit data in these explicitly
 * selected modes is little-endian. Both are overridable for testing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "apusb_modes.h"

#include <string.h>
#include <strings.h>

static const apusb_mode kModes[] = {
    { "24bit-48k", "24-bit up to 48 kHz: analog in + analog out",
      APUSB_IF_ANALOG_OUT, APUSB_IF_ANALOG_IN, 3, APUSB_FMT_S24_3BE,
      2, { 44100.0, 48000.0 }, 48000.0 },
    { "24bit-48k-spdif-out", "24-bit up to 48 kHz: analog in + S/PDIF out",
      APUSB_IF_DIGITAL_OUT, APUSB_IF_ANALOG_IN, 3, APUSB_FMT_S24_3BE,
      2, { 44100.0, 48000.0 }, 48000.0 },
    { "24bit-48k-spdif-in", "24-bit up to 48 kHz: S/PDIF in (active source required) + analog out",
      APUSB_IF_ANALOG_OUT, APUSB_IF_DIGITAL_IN, 2, APUSB_FMT_S24_3BE,
      2, { 44100.0, 48000.0 }, 48000.0 },
    { "16bit-48k", "16-bit up to 48 kHz: analog in + analog out",
      APUSB_IF_ANALOG_OUT, APUSB_IF_ANALOG_IN, 5, APUSB_FMT_S16LE,
      2, { 44100.0, 48000.0 }, 48000.0 },
    { "16bit-48k-spdif-out", "16-bit up to 48 kHz: analog in + S/PDIF out",
      APUSB_IF_DIGITAL_OUT, APUSB_IF_ANALOG_IN, 5, APUSB_FMT_S16LE,
      2, { 44100.0, 48000.0 }, 48000.0 },
    { "16bit-48k-spdif-in", "16-bit up to 48 kHz: S/PDIF in (active source required) + analog out",
      APUSB_IF_ANALOG_OUT, APUSB_IF_DIGITAL_IN, 4, APUSB_FMT_S16LE,
      2, { 44100.0, 48000.0 }, 48000.0 },
    { "24bit-96k-play", "24-bit 88.2/96 kHz: analog out only (half duplex)",
      APUSB_IF_ANALOG_OUT, -1, 1, APUSB_FMT_S24_3BE,
      2, { 88200.0, 96000.0 }, 96000.0 },
    { "24bit-96k-record", "24-bit 88.2/96 kHz: analog in only (half duplex)",
      -1, APUSB_IF_ANALOG_IN, 1, APUSB_FMT_S24_3BE,
      2, { 88200.0, 96000.0 }, 96000.0 },
};

unsigned apusb_mode_count(void) { return (unsigned)(sizeof kModes / sizeof kModes[0]); }

const apusb_mode *apusb_mode_at(unsigned index)
{
    return index < apusb_mode_count() ? &kModes[index] : NULL;
}

const apusb_mode *apusb_mode_by_name(const char *name)
{
    if (name == NULL) return NULL;
    for (unsigned i = 0; i < apusb_mode_count(); i++)
        if (strcasecmp(kModes[i].name, name) == 0) return &kModes[i];
    return NULL;
}

const apusb_mode *apusb_mode_default(void) { return &kModes[0]; }

bool apusb_mode_supports_rate(const apusb_mode *mode, double rate)
{
    if (mode == NULL) return false;
    for (unsigned i = 0; i < mode->numRates; i++)
        if (mode->rates[i] == rate) return true;
    return false;
}

unsigned apusb_format_bytes(apusb_format fmt)
{
    switch (fmt) {
    case APUSB_FMT_S16LE: case APUSB_FMT_S16BE: return 2;
    case APUSB_FMT_S24_3LE: case APUSB_FMT_S24_3BE: return 3;
    }
    return 0;
}

const char *apusb_format_name(apusb_format fmt)
{
    switch (fmt) {
    case APUSB_FMT_S16LE: return "s16le";
    case APUSB_FMT_S16BE: return "s16be";
    case APUSB_FMT_S24_3LE: return "s24le";
    case APUSB_FMT_S24_3BE: return "s24be";
    }
    return "?";
}

bool apusb_format_parse(const char *s, apusb_format *out)
{
    static const struct { const char *n; apusb_format f; } t[] = {
        { "s16le", APUSB_FMT_S16LE }, { "s16be", APUSB_FMT_S16BE },
        { "s24le", APUSB_FMT_S24_3LE }, { "s24be", APUSB_FMT_S24_3BE },
    };
    if (s == NULL || out == NULL) return false;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcasecmp(s, t[i].n) == 0) { *out = t[i].f; return true; }
    return false;
}
