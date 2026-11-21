/*
 * Unit tests for the platform-independent parts (runs on macOS or Linux):
 *   cc -std=c11 -Isrc/common tests/test_portable.c src/common/apusb_{modes,convert,ring,midiman}.c -lm
 * SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "apusb_convert.h"
#include "apusb_midiman.h"
#include "apusb_modes.h"
#include "apusb_ring.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void test_convert(void)
{
    float in[4] = { 0.5f, -1.0f, 1.0f, 0.0f };
    uint8_t b[12];
    apusb_float_to_pcm(in, b, 2, 2, APUSB_FMT_S24_3BE);
    CHECK(b[0] == 0x40 && b[1] == 0x00 && b[2] == 0x00);   /* 0.5 -> 0x400000 BE */
    CHECK(b[3] == 0x80 && b[4] == 0x00 && b[5] == 0x00);   /* -1  -> 0x800000 */
    CHECK(b[6] == 0x7f && b[7] == 0xff && b[8] == 0xff);   /* +1 clips */
    float back[4];
    apusb_pcm_to_float(b, back, 2, 2, APUSB_FMT_S24_3BE);
    CHECK(fabsf(back[0] - 0.5f) < 1e-6f && back[1] == -1.0f && back[3] == 0.0f);

    apusb_float_to_pcm(in, b, 2, 2, APUSB_FMT_S16LE);
    CHECK(b[0] == 0x00 && b[1] == 0x40);
    apusb_float_to_pcm(in, b, 2, 2, APUSB_FMT_S16BE);
    CHECK(b[0] == 0x40 && b[1] == 0x00);
    apusb_float_to_pcm(in, b, 2, 2, APUSB_FMT_S24_3LE);
    CHECK(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x40);

    for (int f = 0; f < 4; f++) {
        float x[2] = { -0.123456f, 0.987654f }, y[2];
        apusb_float_to_pcm(x, b, 1, 2, (apusb_format)f);
        apusb_pcm_to_float(b, y, 1, 2, (apusb_format)f);
        CHECK(fabsf(x[0] - y[0]) < 1e-4f && fabsf(x[1] - y[1]) < 1e-4f);
    }
}

static void test_pacer(void)
{
    apusb_pacer p;
    apusb_pacer_init(&p, 44100);
    unsigned total = 0;
    for (int i = 0; i < 1000; i++) total += apusb_pacer_next(&p, 0);
    CHECK(total == 44100);
    apusb_pacer_init(&p, 48000);
    CHECK(apusb_pacer_next(&p, 0) == 48 && apusb_pacer_next(&p, 1) == 49 && apusb_pacer_next(&p, -1) == 47);
    CHECK(apusb_pacer_max_frames(96000) * 6 <= 1023); /* fits full-speed isoch max */
}

static void test_ring(void)
{
    apusb_ring r;
    CHECK(apusb_ring_init(&r, 8, 2));
    float w[12], rd[12];
    for (int i = 0; i < 12; i++) w[i] = (float)i;
    apusb_ring_write(&r, 6, w, 6); /* wraps */
    apusb_ring_read(&r, 6, rd, 6, true);
    CHECK(memcmp(w, rd, sizeof w) == 0);
    apusb_ring_read(&r, 6, rd, 6, false);
    CHECK(rd[0] == 0.0f && rd[11] == 0.0f);
    CHECK(!apusb_ring_init(&r, 6, 2) || 0); /* non power of two rejected */
    apusb_ring_free(&r);
}

typedef struct { uint8_t bytes[64]; unsigned n; unsigned ports; } sinkbuf;
static void sink(void *c, unsigned port, const uint8_t *b, unsigned n)
{
    sinkbuf *s = (sinkbuf *)c;
    memcpy(s->bytes + s->n, b, n);
    s->n += n;
    s->ports |= 1u << port;
}

static void test_midiman(void)
{
    apusb_midiman_encoder e;
    apusb_midiman_encoder_init(&e, 0);
    const uint8_t msg[] = { 0x90, 60, 100, 62, 90, 0xf8, 0xc0, 5, 0xf0, 1, 2, 3, 4, 0xf7 };
    uint8_t out[128];
    size_t n = apusb_midiman_encode(&e, msg, sizeof msg, out, sizeof out);
    CHECK(n % 4 == 0);
    /* note on, running status note (status re-inserted), clock, prog change, sysex 3+3 */
    CHECK(out[0] == 0x90 && out[1] == 60 && out[2] == 100 && out[3] == 0x03);
    CHECK(out[4] == 0x90 && out[5] == 62 && out[6] == 90 && out[7] == 0x03);
    CHECK(out[8] == 0xf8 && out[11] == 0x01);
    CHECK(out[12] == 0xc0 && out[13] == 5 && out[15] == 0x02);
    CHECK(out[16] == 0xf0 && out[17] == 1 && out[18] == 2 && out[19] == 0x03);
    CHECK(out[20] == 3 && out[21] == 4 && out[22] == 0xf7 && out[23] == 0x03);
    CHECK(n == 24);

    sinkbuf s = { .n = 0 };
    const uint8_t in[] = { 0x90, 64, 1, 0x03, 0, 0, 0, 0, 0xfe, 0, 0, 0x11 };
    apusb_midiman_decode(in, sizeof in, sink, &s);
    CHECK(s.n == 4 && s.bytes[0] == 0x90 && s.bytes[3] == 0xfe && s.ports == 0x3);
}

static void test_modes(void)
{
    CHECK(apusb_mode_count() >= 6);
    const apusb_mode *m = apusb_mode_by_name("24bit-48k");
    CHECK(m && m->altSetting == 3 && m->format == APUSB_FMT_S24_3BE);
    m = apusb_mode_by_name("16bit-48k-spdif-in");
    CHECK(m && m->altSetting == 4 && m->inInterface == APUSB_IF_DIGITAL_IN);
    for (unsigned i = 0; i < apusb_mode_count(); i++) {
        m = apusb_mode_at(i);
        /* documented: interface 5 has no alt 3 or 5 */
        if (m->inInterface == APUSB_IF_DIGITAL_IN) CHECK(m->altSetting != 3 && m->altSetting != 5);
        /* documented: 96 kHz is half duplex */
        if (m->altSetting == 1) CHECK(m->inInterface < 0 || m->outInterface < 0);
        CHECK(apusb_mode_supports_rate(m, m->defaultRate));
    }
    apusb_format f;
    CHECK(apusb_format_parse("S24BE", &f) && f == APUSB_FMT_S24_3BE);
}

int main(void)
{
    test_convert();
    test_pacer();
    test_ring();
    test_midiman();
    test_modes();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("all portable tests passed\n");
    return 0;
}
