/*
 * apusb-diag - command line diagnostics and test tool for the M-Audio
 * Audiophile USB community driver.
 *
 *   apusb-diag list                 find M-Audio USB devices (normal / DFU)
 *   apusb-diag descriptors          dump and decode the USB descriptors
 *   apusb-diag claims               show which kernel drivers own the interfaces
 *   apusb-diag modes                list driver modes
 *   apusb-diag tone   [options]     play a 1 kHz test tone directly over USB
 *   apusb-diag record [options]     record directly over USB to a WAV file
 *   apusb-diag midi-monitor [-s N]  print incoming MIDI for N seconds
 *   apusb-diag midi-test            send a short note on the MIDI output
 *   apusb-diag get-mode             read the HAL driver's mode
 *   apusb-diag set-mode NAME        change the HAL driver's mode
 *
 * tone/record options: --mode NAME --rate HZ --seconds N --format s24be|s24le|s16le|s16be
 *                      --out FILE (record) --seize (take interfaces from another driver)
 *
 * tone/record/midi talk to the hardware directly and must not run while
 * Core Audio is using the device.
 *
 * EXPERIMENTAL - UNTESTED ON HARDWARE.
 * SPDX-License-Identifier: MIT
 */
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apusb_convert.h"
#include "apusb_midiman.h"
#include "apusb_modes.h"
#include "apusb_stream.h"
#include "apusb_usb.h"

#define kDeviceUID "AudiophileUSB_0763_2003"
#define kPropMode  'apmd'
#define kPropModes 'apms'

static void usage(void)
{
    fprintf(stderr,
        "usage: apusb-diag <command> [options]\n"
        "  list | descriptors | claims | modes\n"
        "  tone   [--mode M] [--rate HZ] [--seconds N] [--format F] [--seize]\n"
        "  record [--mode M] [--rate HZ] [--seconds N] [--format F] [--seize] [--out FILE]\n"
        "  midi-monitor [--seconds N] | midi-test\n"
        "  get-mode | set-mode NAME\n");
}

/* ------------------------------------------------------------------------ */

static int open_device(apusb_device *d, bool quiet)
{
    io_service_t s = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
    if (!s) {
        io_service_t dfu = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID_DFU);
        if (dfu) {
            IOObjectRelease(dfu);
            fprintf(stderr, "Device found in DFU / firmware-update mode (0763:2803). It needs its firmware\n"
                            "loaded before it can be used; this driver cannot do that.\n");
        } else if (!quiet) {
            fprintf(stderr, "Audiophile USB (0763:2003) not found. Is it connected and powered?\n");
        }
        return 1;
    }
    IOReturn kr = apusb_device_create(s, d);
    IOObjectRelease(s);
    if (kr) { fprintf(stderr, "cannot create device interface: %s\n", apusb_strerror(kr)); return 1; }
    return 0;
}

static int cmd_list(void)
{
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    SInt32 v = APUSB_VENDOR_ID;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFRelease(nv);
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, m, &it) != kIOReturnSuccess) return 1;
    io_service_t s;
    int count = 0;
    while ((s = IOIteratorNext(it))) {
        apusb_device d;
        if (apusb_device_create(s, &d) == kIOReturnSuccess) {
            io_name_t name = "";
            IORegistryEntryGetName(s, name);
            const char *note = d.productID == APUSB_PRODUCT_ID ? "Audiophile USB (supported)"
                             : d.productID == APUSB_PRODUCT_ID_DFU ? "Audiophile USB in DFU mode (needs firmware, unsupported)"
                             : "other M-Audio device";
            UInt8 cls = 0, sub = 0, proto = 0;
            UInt16 rel = 0;
            (*d.dev)->GetDeviceClass(d.dev, &cls);
            (*d.dev)->GetDeviceSubClass(d.dev, &sub);
            (*d.dev)->GetDeviceProtocol(d.dev, &proto);
            (*d.dev)->GetDeviceReleaseNumber(d.dev, &rel);
            printf("%04x:%04x  \"%s\"  location 0x%08x  class %02x/%02x/%02x  bcdDevice %x.%02x  -> %s\n",
                   d.vendorID, d.productID, name, (unsigned)d.locationID, cls, sub, proto,
                   rel >> 8, rel & 0xff, note);
            apusb_device_release(&d);
            count++;
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (count == 0) printf("No M-Audio (vendor 0x0763) USB devices found.\n");
    return 0;
}

/* ------------------------------------------------------------------------ */

static void hexdump(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        printf("  %04zx:", i);
        for (size_t j = i; j < i + 16 && j < n; j++) printf(" %02x", p[j]);
        printf("\n");
    }
}

static int cmd_descriptors(void)
{
    apusb_device d;
    if (open_device(&d, false)) return 1;
    UInt8 cls = 0, sub = 0, proto = 0, ncfg = 0, cur = 0;
    UInt16 rel = 0;
    (*d.dev)->GetDeviceClass(d.dev, &cls);
    (*d.dev)->GetDeviceSubClass(d.dev, &sub);
    (*d.dev)->GetDeviceProtocol(d.dev, &proto);
    (*d.dev)->GetDeviceReleaseNumber(d.dev, &rel);
    (*d.dev)->GetNumberOfConfigurations(d.dev, &ncfg);
    (*d.dev)->GetConfiguration(d.dev, &cur);
    printf("Device %04x:%04x class %02x/%02x/%02x bcdDevice %04x, %u configuration(s), current %u\n",
           d.vendorID, d.productID, cls, sub, proto, rel, ncfg, cur);

    for (UInt8 c = 0; c < ncfg; c++) {
        IOUSBConfigurationDescriptorPtr cfg = NULL;
        if ((*d.dev)->GetConfigurationDescriptorPtr(d.dev, c, &cfg) != kIOReturnSuccess || !cfg) continue;
        const uint8_t *p = (const uint8_t *)cfg;
        size_t total = (size_t)p[2] | ((size_t)p[3] << 8);
        printf("\nConfiguration %u (bConfigurationValue %u, %zu bytes, %u interfaces, attributes 0x%02x, %u mA)\n",
               c, cfg->bConfigurationValue, total, cfg->bNumInterfaces, cfg->bmAttributes, cfg->MaxPower * 2);
        hexdump(p, total);
        printf("\nDecoded:\n");
        int curClass = -1, curSub = -1;
        for (size_t off = 0; off + 2 <= total; ) {
            uint8_t len = p[off], type = p[off + 1];
            if (len < 2 || off + len > total) break;
            const uint8_t *x = p + off;
            if (type == 4 && len >= 9) {
                curClass = x[5]; curSub = x[6];
                printf("  Interface %u alt %u: %u endpoint(s), class %02x/%02x/%02x%s\n", x[2], x[3], x[4], x[5], x[6], x[7],
                       x[5] == 0xff ? " (vendor specific)" : x[5] == 1 && x[6] == 1 ? " (AudioControl)"
                       : x[5] == 1 && x[6] == 2 ? " (AudioStreaming)" : x[5] == 1 && x[6] == 3 ? " (MIDIStreaming)" : "");
            } else if (type == 5 && len >= 7) {
                static const char *tt[] = { "control", "isochronous", "bulk", "interrupt" };
                static const char *sync[] = { "none", "async", "adaptive", "sync" };
                unsigned mps = x[4] | (x[5] << 8);
                printf("    Endpoint 0x%02x %s %s, sync %s, maxPacket %u, interval %u\n", x[2],
                       (x[2] & 0x80) ? "IN" : "OUT", tt[x[3] & 3], sync[(x[3] >> 2) & 3], mps, x[6]);
            } else if (type == 0x24 && curClass == 1 && curSub == 2 && len >= 3) {
                if (x[2] == 1 && len >= 7)
                    printf("    AS_GENERAL: terminal link %u, delay %u, wFormatTag 0x%04x\n", x[3], x[4], x[5] | (x[6] << 8));
                else if (x[2] == 2 && len >= 8) {
                    printf("    FORMAT_TYPE %u: %u channel(s), subframe %u bytes, %u bits, rates:", x[3], x[4], x[5], x[6]);
                    unsigned n = x[7];
                    if (n == 0 && len >= 14)
                        printf(" continuous %u..%u", x[8] | (x[9] << 8) | (x[10] << 16), x[11] | (x[12] << 8) | (x[13] << 16));
                    for (unsigned i = 0; i < n && 8 + 3 * i + 2 < len; i++)
                        printf(" %u", x[8 + 3 * i] | (x[9 + 3 * i] << 8) | (x[10 + 3 * i] << 16));
                    printf("\n");
                } else {
                    printf("    CS_INTERFACE subtype 0x%02x (%u bytes)\n", x[2], len);
                }
            } else if (type == 0x24) {
                printf("    CS_INTERFACE subtype 0x%02x (%u bytes)\n", len >= 3 ? x[2] : 0, len);
            } else if (type == 0x25 && len >= 4) {
                printf("      CS_ENDPOINT: attributes 0x%02x%s\n", x[3], (x[3] & 1) ? " (sampling-frequency control)" : "");
            } else {
                printf("  descriptor type 0x%02x, %u bytes\n", type, len);
            }
            off += len;
        }
    }
    apusb_device_release(&d);
    return 0;
}

/* ------------------------------------------------------------------------ */

static void print_children(io_registry_entry_t e, int depth)
{
    io_iterator_t it = 0;
    if (IORegistryEntryGetChildIterator(e, kIOServicePlane, &it) != kIOReturnSuccess) return;
    io_registry_entry_t c;
    while ((c = IOIteratorNext(it))) {
        io_name_t cls = "", name = "";
        IOObjectGetClass(c, cls);
        IORegistryEntryGetName(c, name);
        CFTypeRef num = IORegistryEntryCreateCFProperty(c, CFSTR("bInterfaceNumber"), NULL, 0);
        int ifn = -1;
        if (num && CFGetTypeID(num) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)num, kCFNumberIntType, &ifn);
        if (num) CFRelease(num);
        printf("%*s%s \"%s\"", depth * 2, "", cls, name);
        if (ifn >= 0) printf("  [interface %d]", ifn);
        printf("\n");
        if (depth < 4) print_children(c, depth + 1);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
}

static int cmd_claims(void)
{
    io_service_t s = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
    if (!s) s = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID_DFU);
    if (!s) { fprintf(stderr, "device not found\n"); return 1; }
    io_name_t cls = "";
    IOObjectGetClass(s, cls);
    printf("%s (device)\n", cls);
    print_children(s, 1);
    printf("\nIf an interface (2-5) has a child such as AppleUSBAudioDevice/AppleUSBAudioEngine,\n"
           "the built-in macOS USB audio driver owns it and this user-space driver cannot\n"
           "open it without --seize (see README 'Troubleshooting').\n");
    IOObjectRelease(s);
    return 0;
}

static int cmd_modes(void)
{
    for (unsigned i = 0; i < apusb_mode_count(); i++) {
        const apusb_mode *m = apusb_mode_at(i);
        printf("%-22s alt %u, %s, out-if %d, in-if %d, rates", m->name, m->altSetting,
               apusb_format_name(m->format), m->outInterface, m->inInterface);
        for (unsigned r = 0; r < m->numRates; r++) printf(" %.0f", m->rates[r]);
        printf("\n    %s%s\n", m->description, m == apusb_mode_default() ? " [default]" : "");
    }
    return 0;
}

/* ------------------------------------------------------------------------ */

typedef struct {
    const char  *mode;
    double       rate;
    double       seconds;
    bool         haveFormat;
    apusb_format format;
    bool         seize;
    const char  *out;
} stream_opts;

typedef struct {
    apusb_format fmt;
    apusb_pacer  pacer;
    double       phase, step;
    FILE        *wav;
    uint64_t     framesRecorded;
    float        peak;
} stream_ctx;

static unsigned tone_frames(void *c) { return apusb_pacer_next(&((stream_ctx *)c)->pacer, 0); }

static void tone_fill(void *c, uint8_t *dst, unsigned frames)
{
    stream_ctx *x = (stream_ctx *)c;
    float buf[2 * 128];
    while (frames) {
        unsigned n = frames > 128 ? 128 : frames;
        for (unsigned i = 0; i < n; i++) {
            float v = (float)(0.25 * sin(x->phase));
            x->phase += x->step;
            if (x->phase > 2 * M_PI) x->phase -= 2 * M_PI;
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        apusb_float_to_pcm(buf, dst, n, 2, x->fmt);
        dst += (size_t)n * 2 * apusb_format_bytes(x->fmt);
        frames -= n;
    }
}

static void rec_consume(void *c, const uint8_t *src, unsigned frames)
{
    stream_ctx *x = (stream_ctx *)c;
    float buf[2 * 128];
    uint8_t le[2 * 128 * 3];
    while (frames) {
        unsigned n = frames > 128 ? 128 : frames;
        apusb_pcm_to_float(src, buf, n, 2, x->fmt);
        for (unsigned i = 0; i < 2 * n; i++) if (fabsf(buf[i]) > x->peak) x->peak = fabsf(buf[i]);
        apusb_float_to_pcm(buf, le, n, 2, APUSB_FMT_S24_3LE);
        if (x->wav) fwrite(le, 6, n, x->wav);
        x->framesRecorded += n;
        src += (size_t)n * 2 * apusb_format_bytes(x->fmt);
        frames -= n;
    }
}

static void status_cb(void *c, IOReturn r, const char *what)
{
    (void)c;
    fprintf(stderr, "  [usb] %s: %s\n", what, apusb_strerror(r));
}

static void put_le(FILE *f, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) fputc((v >> (8 * i)) & 0xff, f); }

static void wav_header(FILE *f, uint32_t rate, uint32_t frames)
{
    uint32_t data = frames * 6;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put_le(f, 36 + data, 4); fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4); put_le(f, 1, 2); put_le(f, 2, 2); put_le(f, rate, 4);
    put_le(f, rate * 6, 4); put_le(f, 6, 2); put_le(f, 24, 2);
    fwrite("data", 1, 4, f); put_le(f, data, 4);
}

static int run_stream(bool capture, const stream_opts *o)
{
    const apusb_mode *mode = apusb_mode_by_name(o->mode);
    if (!mode) { fprintf(stderr, "unknown mode '%s' (see 'apusb-diag modes')\n", o->mode); return 2; }
    int ifnum = capture ? mode->inInterface : mode->outInterface;
    if (ifnum < 0) { fprintf(stderr, "mode %s has no %s port\n", mode->name, capture ? "input" : "output"); return 2; }
    double rate = o->rate > 0 ? o->rate : mode->defaultRate;
    if (!apusb_mode_supports_rate(mode, rate)) fprintf(stderr, "warning: %.0f Hz not listed for mode %s\n", rate, mode->name);

    stream_ctx ctx = { .fmt = o->haveFormat ? o->format : mode->format };
    apusb_pacer_init(&ctx.pacer, (uint32_t)rate);
    ctx.step = 2 * M_PI * 1000.0 / rate;

    apusb_device d;
    if (open_device(&d, false)) return 1;
    IOReturn kr = apusb_device_ensure_configured(&d);
    if (kr) fprintf(stderr, "ensure configuration: %s (continuing)\n", apusb_strerror(kr));

    apusb_interface intf[APUSB_IF_STREAM_LAST + 1];
    memset(intf, 0, sizeof intf);
    for (int i = APUSB_IF_STREAM_FIRST; i <= APUSB_IF_STREAM_LAST; i++) {
        kr = apusb_interface_open(&d, (UInt8)i, o->seize, &intf[i]);
        printf("open interface %d: %s\n", i, apusb_strerror(kr));
        if (!kr) apusb_interface_set_alt(&intf[i], 0);
    }
    int rc = 1;
    apusb_stream *s = NULL;
    apusb_interface *it = &intf[ifnum];
    apusb_pipe pipe;
    if (!it->intf) goto out;
    if ((kr = apusb_interface_set_alt(it, mode->altSetting))) { fprintf(stderr, "set alt %u: %s\n", mode->altSetting, apusb_strerror(kr)); goto out; }
    if ((kr = apusb_interface_find_pipe(it, capture ? kUSBIn : kUSBOut, kUSBIsoc, &pipe))) { fprintf(stderr, "find pipe: %s\n", apusb_strerror(kr)); goto out; }
    printf("interface %d alt %u: endpoint 0x%02x maxPacket %u, format %s, %.0f Hz\n",
           ifnum, mode->altSetting, pipe.address, pipe.maxPacketSize, apusb_format_name(ctx.fmt), rate);
    kr = apusb_set_sample_rate(it, pipe.address, (UInt32)rate);
    printf("SET_CUR sample rate: %s\n", apusb_strerror(kr));
    UInt32 readBack = 0;
    kr = apusb_get_sample_rate(it, pipe.address, &readBack);
    printf("GET_CUR sample rate: %s%s%u\n", apusb_strerror(kr), kr ? "" : " -> ", kr ? 0 : readBack);

    if (capture) {
        const char *path = o->out ? o->out : "apusb-record.wav";
        ctx.wav = fopen(path, "wb");
        if (!ctx.wav) { perror(path); goto out; }
        wav_header(ctx.wav, (uint32_t)rate, 0);
    }
    apusb_stream_callbacks cb = { .packet_frames = tone_frames, .fill = tone_fill,
                                  .consume = rec_consume, .status = status_cb, .ctx = &ctx };
    s = apusb_stream_create(it, &pipe, capture, 2 * apusb_format_bytes(ctx.fmt), &cb);
    if (!s) { fprintf(stderr, "cannot create stream\n"); goto out; }
    if ((kr = apusb_stream_start(s))) { fprintf(stderr, "start stream: %s\n", apusb_strerror(kr)); goto out; }
    printf("%s for %.1f s ...\n", capture ? "recording" : "playing 1 kHz tone at -12 dBFS", o->seconds);
    usleep((useconds_t)(o->seconds * 1e6));
    apusb_stream_stop(s);
    uint64_t pk = 0, er = 0, rs = 0;
    apusb_stream_stats(s, &pk, &er, &rs);
    printf("USB packets %llu, errors %llu, resyncs %llu\n", (unsigned long long)pk, (unsigned long long)er, (unsigned long long)rs);
    if (capture) {
        printf("recorded %llu frames (%.2f s), peak %.1f dBFS -> %s\n", (unsigned long long)ctx.framesRecorded,
               ctx.framesRecorded / rate, ctx.peak > 0 ? 20 * log10(ctx.peak) : -200.0, o->out ? o->out : "apusb-record.wav");
        printf("If the recording sounds like loud noise, retry with a different --format (byte order).\n");
    } else {
        printf("If you heard noise instead of a clean tone, retry with a different --format (byte order).\n");
    }
    rc = er ? 3 : 0;
out:
    if (s) apusb_stream_destroy(s);
    if (ctx.wav) { wav_header(ctx.wav, (uint32_t)rate, (uint32_t)ctx.framesRecorded); fclose(ctx.wav); }
    for (int i = APUSB_IF_STREAM_FIRST; i <= APUSB_IF_STREAM_LAST; i++) {
        if (intf[i].intf) { apusb_interface_set_alt(&intf[i], 0); apusb_interface_close(&intf[i]); }
    }
    apusb_device_release(&d);
    return rc;
}

/* ------------------------------------------------------------------------ */

static int open_midi(apusb_device *d, apusb_interface *i, apusb_pipe *in, apusb_pipe *out)
{
    if (open_device(d, false)) return 1;
    apusb_device_ensure_configured(d);
    IOReturn kr = apusb_interface_open(d, APUSB_IF_MIDI, false, i);
    if (kr) { fprintf(stderr, "open MIDI interface 6: %s (is the CoreMIDI driver using it?)\n", apusb_strerror(kr)); apusb_device_release(d); return 1; }
    if (apusb_interface_find_pipe(i, kUSBIn, kUSBInterrupt, in) || apusb_interface_find_pipe(i, kUSBOut, kUSBBulk, out)) {
        fprintf(stderr, "MIDI endpoints not found as expected (interrupt IN + bulk OUT)\n");
        apusb_interface_close(i); apusb_device_release(d); return 1;
    }
    printf("MIDI: IN endpoint 0x%02x (%u), OUT endpoint 0x%02x (%u)\n", in->address, in->maxPacketSize, out->address, out->maxPacketSize);
    return 0;
}

static void print_midi(void *ctx, unsigned port, const uint8_t *b, unsigned n)
{
    (void)ctx;
    printf("  port %u:", port);
    for (unsigned i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
    fflush(stdout);
}

static int cmd_midi_monitor(double seconds)
{
    apusb_device d; apusb_interface i; apusb_pipe in, out;
    if (open_midi(&d, &i, &in, &out)) return 1;
    printf("listening for %.0f s - play something on the MIDI input...\n", seconds);
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < end) {
        uint8_t buf[64];
        UInt32 size = in.maxPacketSize < sizeof buf ? in.maxPacketSize : sizeof buf;
        IOReturn kr = (*i.intf)->ReadPipeTO(i.intf, in.pipeRef, buf, &size, 500, 1000);
        if (kr == kIOReturnSuccess && size) {
            printf("raw:");
            for (UInt32 k = 0; k < size; k++) printf(" %02x", buf[k]);
            printf("\n");
            apusb_midiman_decode(buf, size, print_midi, NULL);
        } else if (kr != kIOReturnSuccess && kr != kIOUSBTransactionTimeout && kr != kIOReturnTimeout) {
            fprintf(stderr, "read: %s\n", apusb_strerror(kr));
            (*i.intf)->ClearPipeStallBothEnds(i.intf, in.pipeRef);
        }
    }
    apusb_interface_close(&i);
    apusb_device_release(&d);
    return 0;
}

static int cmd_midi_test(void)
{
    apusb_device d; apusb_interface i; apusb_pipe in, out;
    if (open_midi(&d, &i, &in, &out)) return 1;
    apusb_midiman_encoder e;
    apusb_midiman_encoder_init(&e, 0);
    const uint8_t on[] = { 0x90, 60, 100 }, off[] = { 0x80, 60, 0 };
    uint8_t pkt[16];
    size_t n = apusb_midiman_encode(&e, on, sizeof on, pkt, sizeof pkt);
    IOReturn kr = (*i.intf)->WritePipe(i.intf, out.pipeRef, pkt, (UInt32)n);
    printf("note on (middle C): %s\n", apusb_strerror(kr));
    usleep(500000);
    n = apusb_midiman_encode(&e, off, sizeof off, pkt, sizeof pkt);
    kr = (*i.intf)->WritePipe(i.intf, out.pipeRef, pkt, (UInt32)n);
    printf("note off: %s\n", apusb_strerror(kr));
    apusb_interface_close(&i);
    apusb_device_release(&d);
    return kr ? 1 : 0;
}

/* ------------------------------------------------------------------------ */

static AudioObjectID find_hal_device(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice,
                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef uid = CFSTR(kDeviceUID);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &dev) != noErr)
        return kAudioObjectUnknown;
    return dev;
}

static int cmd_get_mode(void)
{
    AudioObjectID dev = find_hal_device();
    if (dev == kAudioObjectUnknown) { fprintf(stderr, "HAL driver device not found (driver not installed or device unplugged)\n"); return 1; }
    AudioObjectPropertyAddress a = { kPropMode, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 size = sizeof s;
    OSStatus err = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &s);
    if (err != noErr || !s) { fprintf(stderr, "get mode failed: %d\n", (int)err); return 1; }
    char buf[64];
    CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
    printf("%s\n", buf);
    CFRelease(s);
    return 0;
}

static int cmd_set_mode(const char *name)
{
    if (!apusb_mode_by_name(name)) { fprintf(stderr, "unknown mode '%s'\n", name); return 2; }
    AudioObjectID dev = find_hal_device();
    if (dev == kAudioObjectUnknown) { fprintf(stderr, "HAL driver device not found\n"); return 1; }
    AudioObjectPropertyAddress a = { kPropMode, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    OSStatus err = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof s, &s);
    CFRelease(s);
    if (err != noErr) { fprintf(stderr, "set mode failed: %d\n", (int)err); return 1; }
    printf("mode set to %s. Power-cycle the interface if audio is distorted after a mode change.\n", name);
    return 0;
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    stream_opts o = { .mode = apusb_mode_default()->name, .seconds = 5.0 };
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--mode") && v) { o.mode = v; i++; }
        else if (!strcmp(a, "--rate") && v) { o.rate = atof(v); i++; }
        else if ((!strcmp(a, "--seconds") || !strcmp(a, "-s")) && v) { o.seconds = atof(v); i++; }
        else if (!strcmp(a, "--format") && v) {
            if (!apusb_format_parse(v, &o.format)) { fprintf(stderr, "bad format %s\n", v); return 2; }
            o.haveFormat = true; i++;
        }
        else if (!strcmp(a, "--out") && v) { o.out = v; i++; }
        else if (!strcmp(a, "--seize")) o.seize = true;
        else if (!strcmp(cmd, "set-mode") && i == 2) { /* positional */ }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    if (!strcmp(cmd, "list")) return cmd_list();
    if (!strcmp(cmd, "descriptors")) return cmd_descriptors();
    if (!strcmp(cmd, "claims")) return cmd_claims();
    if (!strcmp(cmd, "modes")) return cmd_modes();
    if (!strcmp(cmd, "tone")) return run_stream(false, &o);
    if (!strcmp(cmd, "record")) return run_stream(true, &o);
    if (!strcmp(cmd, "midi-monitor")) return cmd_midi_monitor(o.seconds);
    if (!strcmp(cmd, "midi-test")) return cmd_midi_test();
    if (!strcmp(cmd, "get-mode")) return cmd_get_mode();
    if (!strcmp(cmd, "set-mode")) { if (argc < 3) { usage(); return 2; } return cmd_set_mode(argv[2]); }
    usage();
    return 2;
}
