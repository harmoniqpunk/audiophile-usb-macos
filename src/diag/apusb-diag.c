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
#include <CoreMIDI/CoreMIDI.h>
#include <notify.h>
#include "apusb_reset.h"
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/resource.h>
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
        "  get-mode | set-mode NAME | reset [--method 1..4]\n"
        "  hal-list                                   Core Audio devices with latency / safety offsets\n"
        "  coremidi-list | coremidi-test [--seconds N]   (through the installed CoreMIDI driver)\n"
        "  hal-test [--uid UID] [--buffer N] [--seconds N] [--level dBFS] [--rate HZ]\n"
        "           [--loopback | --sweep] [--channel 0|1] [--out FILE (input+output dump)]\n");
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
    if (kr != kIOReturnSuccess) {
        /* audiophileusbd holds the device: interfaces can still be opened */
        kr = apusb_device_attach(s, d);
        if (!kr && !quiet) fprintf(stderr, "(device held by audiophileusbd: using interfaces directly)\n");
    }
    IOObjectRelease(s);
    if (kr) { fprintf(stderr, "cannot create device interface: %s\n", apusb_strerror(kr)); return 1; }
    return 0;
}

static int cmd_list(void)
{
    CFMutableDictionaryRef m = apusb_matching(APUSB_VENDOR_ID, 0);
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
    if (!d.dev) {
        fprintf(stderr, "descriptors need the device itself; stop audiophileusbd first:\n"
                        "  sudo launchctl bootout system/com.harmoniqpunk.audiophileusbd\n");
        apusb_device_release(&d);
        return 1;
    }
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
    unsigned     xfers, framesPerXfer;
    const char  *uid;
    unsigned     buffer;
    double       level;
    bool         loopback;
    int          channel;   /* hal-test: -1 = all channels */
    int          method;    /* reset method (apusb_reset.h) */
    bool         sweep;     /* hal-test: log sine sweep 50 Hz .. 15 kHz instead of 1 kHz */
    bool         capture;
} stream_opts;

typedef struct {
    apusb_format fmt;
    apusb_pacer  pacer;
    double       phase, step;
    FILE        *wav;
    uint64_t     framesRecorded;
    float        peak;
    double       amp;
    double       sumsq, sumsqLate;
    uint64_t     framesLate;
} stream_ctx;

static unsigned tone_frames(void *c) { return apusb_pacer_next(&((stream_ctx *)c)->pacer, 0); }

static void tone_fill(void *c, uint8_t *dst, unsigned frames)
{
    stream_ctx *x = (stream_ctx *)c;
    float buf[2 * 128];
    while (frames) {
        unsigned n = frames > 128 ? 128 : frames;
        for (unsigned i = 0; i < n; i++) {
            float v = (float)(x->amp * sin(x->phase));
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
        for (unsigned i = 0; i < 2 * n; i++) { if (fabsf(buf[i]) > x->peak) x->peak = fabsf(buf[i]); x->sumsq += (double)buf[i] * buf[i]; }
        if (x->framesRecorded >= 4800) { /* skip the first ~100 ms (start-up transient) */
            for (unsigned i = 0; i < 2 * n; i++) x->sumsqLate += (double)buf[i] * buf[i];
            x->framesLate += n;
        }
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
    ctx.amp = pow(10.0, (o->level != 0 ? o->level : -30.0) / 20.0);

    apusb_device d;
    if (open_device(&d, false)) return 1;
    IOReturn kr;
    if (o->capture && d.dev) {
        kr = apusb_device_capture(&d, true);
        printf("capture device from macOS drivers (needs root): %s\n", apusb_strerror(kr));
        if (kr) { apusb_device_release(&d); return 1; }
    }
    kr = apusb_device_ensure_configured(&d);
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
    s = apusb_stream_create(it, &pipe, capture, 2 * apusb_format_bytes(ctx.fmt), o->xfers, o->framesPerXfer, &cb);
    if (!s) { fprintf(stderr, "cannot create stream\n"); goto out; }
    if ((kr = apusb_stream_start(s))) { fprintf(stderr, "start stream: %s\n", apusb_strerror(kr)); goto out; }
    printf("%s for %.1f s ...\n", capture ? "recording" : "playing 1 kHz tone", o->seconds);
    usleep((useconds_t)(o->seconds * 1e6));
    apusb_stream_stop(s);
    uint64_t pk = 0, er = 0, rs = 0;
    apusb_stream_stats(s, &pk, &er, &rs);
    printf("USB packets %llu, errors %llu, resyncs %llu\n", (unsigned long long)pk, (unsigned long long)er, (unsigned long long)rs);
    if (capture) {
        printf("recorded %llu frames (%.2f s), peak %.1f dBFS -> %s\n", (unsigned long long)ctx.framesRecorded,
               ctx.framesRecorded / rate, ctx.peak > 0 ? 20 * log10(ctx.peak) : -200.0, o->out ? o->out : "apusb-record.wav");
        double rms = ctx.framesRecorded ? sqrt(ctx.sumsq / (2.0 * ctx.framesRecorded)) : 0;
        printf("RMS %.1f dBFS (silent input: ~-80..-100 if byte order is right, ~-5..-20 if wrong)\n", rms > 0 ? 20 * log10(rms) : -200.0);
        double late = ctx.framesLate ? sqrt(ctx.sumsqLate / (2.0 * ctx.framesLate)) : 0;
        printf("RMS after first 100 ms %.1f dBFS\n", late > 0 ? 20 * log10(late) : -200.0);
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
    if (o->capture && d.dev) printf("release device to macOS: %s\n", apusb_strerror(apusb_device_capture(&d, false)));
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

static int cmd_reset(int method)
{
    int tok = -1; uint64_t before = 0, now = 0;
    if (method > 0) {
        int rt = -1;
        if (notify_register_check(APUSB_NOTIFY_RESET_REQUEST, &rt) == NOTIFY_STATUS_OK) { notify_set_state(rt, (uint64_t)method); notify_cancel(rt); }
    }
    if (notify_register_check(APUSB_NOTIFY_RESET_DONE, &tok) != NOTIFY_STATUS_OK) { fprintf(stderr, "notify failed\n"); return 1; }
    int chk = 0; notify_check(tok, &chk); notify_get_state(tok, &before);
    notify_post(APUSB_NOTIFY_RESET_REQUEST);
    for (int i = 0; i < 200; i++) {
        usleep(50000);
        notify_get_state(tok, &now);
        if (now != before) { printf("device reset by audiophileusbd (method %d, generation %llu)\n", method, (unsigned long long)now); return 0; }
    }
    fprintf(stderr, "no answer from audiophileusbd within 5 s (is it installed/running?)\n");
    return 1;
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
/* Core Audio level tests (work for this driver and for any other device). */

static UInt32 get_u32(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    UInt32 v = 0, size = sizeof v;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &v) != noErr) return 0xffffffffu;
    return v;
}

static UInt32 stream_latency(AudioObjectID dev, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
    AudioObjectID streams[8];
    UInt32 size = sizeof streams;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, streams) != noErr || size == 0) return 0;
    return get_u32(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
}

static void print_device(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef name = NULL, uid = NULL;
    UInt32 size = sizeof name;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &name);
    a.mSelector = kAudioDevicePropertyDeviceUID; size = sizeof uid;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &uid);
    char n[128] = "?", u[256] = "?";
    if (name) { CFStringGetCString(name, n, sizeof n, kCFStringEncodingUTF8); CFRelease(name); }
    if (uid) { CFStringGetCString(uid, u, sizeof u, kCFStringEncodingUTF8); CFRelease(uid); }
    Float64 rate = 0; size = sizeof rate;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
    AudioValueRange br = { 0, 0 }; size = sizeof br;
    a.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &br);
    printf("[%u] \"%s\"  uid=%s  %.0f Hz  buffer %u (range %.0f-%.0f)\n", dev, n, u, rate,
           get_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal), br.mMinimum, br.mMaximum);
    for (int sc = 0; sc < 2; sc++) {
        AudioObjectPropertyScope scope = sc ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
        AudioObjectPropertyAddress sa = { kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
        UInt32 ss = 0;
        if (AudioObjectGetPropertyDataSize(dev, &sa, 0, NULL, &ss) != noErr || ss == 0) continue;
        printf("     %s: device latency %u, stream latency %u, safety offset %u frames\n", sc ? "input " : "output",
               get_u32(dev, kAudioDevicePropertyLatency, scope), stream_latency(dev, scope),
               get_u32(dev, kAudioDevicePropertySafetyOffset, scope));
        AudioObjectID streams[8];
        UInt32 n = sizeof streams;
        if (AudioObjectGetPropertyData(dev, &sa, 0, NULL, &n, streams) == noErr && n >= sizeof(AudioObjectID)) {
            AudioObjectPropertyAddress fa = { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            AudioStreamBasicDescription f;
            UInt32 fs = sizeof f;
            if (AudioObjectGetPropertyData(streams[0], &fa, 0, NULL, &fs, &f) == noErr)
                printf("     physical format: %u-bit %s%s, %u ch, %.0f Hz, flags 0x%x\n", f.mBitsPerChannel,
                       (f.mFormatFlags & kAudioFormatFlagIsFloat) ? "float" : "int",
                       (f.mFormatFlags & kAudioFormatFlagIsBigEndian) ? " big-endian" : "", f.mChannelsPerFrame, f.mSampleRate, f.mFormatFlags);
            fa.mSelector = kAudioStreamPropertyAvailablePhysicalFormats;
            AudioStreamRangedDescription av[32];
            fs = sizeof av;
            if (AudioObjectGetPropertyData(streams[0], &fa, 0, NULL, &fs, av) == noErr) {
                printf("     available:");
                for (UInt32 k = 0; k < fs / sizeof av[0]; k++)
                    printf(" %u-bit@%.0f-%.0f", av[k].mFormat.mBitsPerChannel, av[k].mSampleRateRange.mMinimum, av[k].mSampleRateRange.mMaximum);
                printf("\n");
            }
        }
    }
}

static int cmd_hal_list(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) return 1;
    AudioObjectID devs[64];
    if (size > sizeof devs) size = sizeof devs;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, devs);
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) print_device(devs[i]);
    return 0;
}

typedef struct {
    double      rate, phase, step;
    float       amp;
    bool        loopback, sweep;
    int         channel;
    double      sweepT;
    uint8_t    *rec; size_t recCap, recLen;   /* --out: per-cycle input+output dump */
    _Atomic uint64_t cycles;
    _Atomic uint32_t overloads;
    float       inPeak;
    /* loopback measurement */
    double      nextPulseAt;     /* output sample time of next pulse */
    double      lastPulseOut;    /* output sample time of last pulse */
    bool        waiting;
    double      rtSum; int rtCount; double rtMin, rtMax;
    uint64_t    hostNanosSum, hostNanosMax;
} hal_ctx;

static OSStatus hal_ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                           AudioBufferList *out, const AudioTimeStamp *outTime, void *cd)
{
    (void)dev; (void)now;
    hal_ctx *c = (hal_ctx *)cd;
    uint64_t t0 = mach_absolute_time();
    atomic_fetch_add(&c->cycles, 1);
    if (in && in->mNumberBuffers > 0 && in->mBuffers[0].mData) {
        const float *x = (const float *)in->mBuffers[0].mData;
        UInt32 ch = in->mBuffers[0].mNumberChannels ? in->mBuffers[0].mNumberChannels : 1;
        UInt32 n = in->mBuffers[0].mDataByteSize / (sizeof(float) * ch);
        for (UInt32 i = 0; i < n; i++) {
            float v = fabsf(x[i * ch]);
            if (v > c->inPeak) c->inPeak = v;
            if (c->loopback && c->waiting && v > 0.3f * c->amp) {
                double rt = inTime->mSampleTime + i - c->lastPulseOut;
                if (rt > 0 && rt < c->rate) {
                    c->rtSum += rt; c->rtCount++;
                    if (c->rtCount == 1 || rt < c->rtMin) c->rtMin = rt;
                    if (rt > c->rtMax) c->rtMax = rt;
                }
                c->waiting = false;
            }
        }
    }
    if (out && out->mNumberBuffers > 0 && out->mBuffers[0].mData) {
        float *y = (float *)out->mBuffers[0].mData;
        UInt32 ch = out->mBuffers[0].mNumberChannels ? out->mBuffers[0].mNumberChannels : 1;
        UInt32 n = out->mBuffers[0].mDataByteSize / (sizeof(float) * ch);
        for (UInt32 i = 0; i < n; i++) {
            float v = 0;
            double t = outTime->mSampleTime + i;
            if (c->loopback) {
                if (t >= c->nextPulseAt && t < c->nextPulseAt + 1) {
                    v = c->amp; c->lastPulseOut = t; c->waiting = true; c->nextPulseAt += c->rate / 2;
                }
            } else {
                v = (float)(c->amp * sin(c->phase));
                if (c->sweep) {   /* exponential sweep 50 Hz -> 15 kHz, 1 octave per ~0.36 s, repeating */
                    double f = 50.0 * pow(2.0, fmod(c->sweepT, 3.0) * 8.23 / 3.0);
                    c->sweepT += 1.0 / c->rate;
                    c->phase += 2 * M_PI * f / c->rate;
                } else c->phase += c->step;
                if (c->phase > 2 * M_PI) c->phase -= 2 * M_PI;
            }
            for (UInt32 k = 0; k < ch; k++) y[i * ch + k] = (c->channel < 0 || (int)k == c->channel) ? v : 0.0f;
        }
    }
    if (c->rec && in && out && in->mNumberBuffers > 0 && out->mNumberBuffers > 0 && in->mBuffers[0].mData && out->mBuffers[0].mData &&
        in->mBuffers[0].mDataByteSize == out->mBuffers[0].mDataByteSize) {
        uint32_t bytes = in->mBuffers[0].mDataByteSize;
        size_t need = 24 + 2 * (size_t)bytes;
        if (c->recLen + need <= c->recCap) {
            uint8_t *p = c->rec + c->recLen;
            double ti = inTime->mSampleTime, to = outTime->mSampleTime;
            uint32_t chn = in->mBuffers[0].mNumberChannels;
            memcpy(p, &ti, 8); memcpy(p + 8, &to, 8); memcpy(p + 16, &bytes, 4); memcpy(p + 20, &chn, 4);
            memcpy(p + 24, in->mBuffers[0].mData, bytes); memcpy(p + 24 + bytes, out->mBuffers[0].mData, bytes);
            c->recLen += need;
        }
    }
    uint64_t dt = mach_absolute_time() - t0;
    c->hostNanosSum += dt;
    if (dt > c->hostNanosMax) c->hostNanosMax = dt;
    return noErr;
}

static OSStatus overload_listener(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *cd)
{
    (void)obj; (void)n; (void)a;
    atomic_fetch_add(&((hal_ctx *)cd)->overloads, 1);
    return noErr;
}

static AudioObjectID device_by_uid(const char *uidStr)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef uid = CFStringCreateWithCString(NULL, uidStr, kCFStringEncodingUTF8);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof uid, &uid, &size, &dev);
    CFRelease(uid);
    return dev;
}

static int cmd_hal_test(const char *uid, unsigned buffer, double seconds, bool loopback, double level, double reqRate,
                        int channel, bool sweep, const char *outPath)
{
    AudioObjectID dev = device_by_uid(uid ? uid : kDeviceUID);
    if (dev == kAudioObjectUnknown) { fprintf(stderr, "Core Audio device '%s' not found\n", uid ? uid : kDeviceUID); return 1; }
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (reqRate > 0) {
        AudioObjectPropertyAddress r = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        OSStatus e = AudioObjectSetPropertyData(dev, &r, 0, NULL, sizeof reqRate, &reqRate);
        if (e) fprintf(stderr, "set sample rate %.0f: %d\n", reqRate, (int)e);
        usleep(500000);
    }
    UInt32 bs = buffer;
    OSStatus err = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof bs, &bs);
    if (err) fprintf(stderr, "set buffer size %u: %d\n", buffer, (int)err);
    hal_ctx c = { 0 };
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    UInt32 size = sizeof c.rate;
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &c.rate);
    c.step = 2 * M_PI * 1000.0 / c.rate;
    c.amp = (float)pow(10.0, level / 20.0);
    c.loopback = loopback;
    c.channel = channel; c.sweep = sweep;
    if (outPath) { c.recCap = (size_t)((seconds + 1.5) * c.rate * 2 * 4 * 2 * 1.2) + 1000000; c.rec = malloc(c.recCap); }
    c.nextPulseAt = -1;
    print_device(dev);

    AudioObjectPropertyAddress ov = { kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &ov, overload_listener, &c);
    AudioDeviceIOProcID pid = NULL;
    if ((err = AudioDeviceCreateIOProcID(dev, hal_ioproc, &c, &pid))) { fprintf(stderr, "CreateIOProcID: %d\n", (int)err); return 1; }
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    if ((err = AudioDeviceStart(dev, pid))) { fprintf(stderr, "AudioDeviceStart: %d\n", (int)err); AudioDeviceDestroyIOProcID(dev, pid); return 1; }
    printf("running %s at %.1f dBFS for %.1f s, buffer %u frames...\n", loopback ? "loopback pulses" : "1 kHz tone", level, seconds,
           get_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal));
    c.nextPulseAt = 0; /* set below once time is known */
    AudioTimeStamp ts = { 0 };
    usleep(200000);
    if (AudioDeviceGetCurrentTime(dev, &ts) == noErr) c.nextPulseAt = ts.mSampleTime + c.rate / 4;
    usleep((useconds_t)(seconds * 1e6));
    AudioDeviceStop(dev, pid);
    getrusage(RUSAGE_SELF, &r1);
    AudioDeviceDestroyIOProcID(dev, pid);
    AudioObjectRemovePropertyListener(dev, &ov, overload_listener, &c);

    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t cyc = atomic_load(&c.cycles);
    double cpu = (r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) / 1e6 +
                 (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) / 1e6;
    printf("IO cycles %llu (expected ~%.0f), overloads %u, input peak %.1f dBFS\n", (unsigned long long)cyc,
           seconds * c.rate / (double)bs, atomic_load(&c.overloads), c.inPeak > 0 ? 20 * log10(c.inPeak) : -200.0);
    if (cyc) printf("IOProc time avg %.1f us, max %.1f us; client process CPU %.1f%%\n",
                    (double)c.hostNanosSum * tb.numer / tb.denom / cyc / 1000.0,
                    (double)c.hostNanosMax * tb.numer / tb.denom / 1000.0, 100.0 * cpu / (seconds + 0.2));
    if (c.rec) {
        FILE *f = fopen(outPath, "wb");
        if (f) { fwrite(c.rec, 1, c.recLen, f); fclose(f); printf("wrote %zu bytes of input+output cycles to %s\n", c.recLen, outPath); }
        free(c.rec);
    }
    if (loopback) {
        if (c.rtCount)
            printf("LOOPBACK round trip: avg %.1f frames (%.2f ms), min %.0f, max %.0f over %d pulses\n",
                   c.rtSum / c.rtCount, 1000.0 * c.rtSum / c.rtCount / c.rate, c.rtMin, c.rtMax, c.rtCount);
        else
            printf("LOOPBACK: no pulse detected on input (no cable from output to input?)\n");
    }
    return 0;
}


/* ------------------------------------------------------------------------ */
/* CoreMIDI-level checks: these go through MIDIServer and the installed
 * AudiophileUSBMIDI.plugin, i.e. exactly what a DAW sees. */

static void midi_name(MIDIObjectRef o, char *buf, size_t n)
{
    CFStringRef s = NULL; buf[0] = 0;
    if (MIDIObjectGetStringProperty(o, kMIDIPropertyDisplayName, &s) == noErr && s) {
        CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(s);
    } else if (MIDIObjectGetStringProperty(o, kMIDIPropertyName, &s) == noErr && s) {
        CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8); CFRelease(s);
    }
}

static int cmd_coremidi_list(void)
{
    char name[256], drv[256];
    ItemCount nd = MIDIGetNumberOfDevices();
    printf("CoreMIDI devices: %lu\n", (unsigned long)nd);
    for (ItemCount i = 0; i < nd; i++) {
        MIDIDeviceRef d = MIDIGetDevice(i);
        SInt32 offline = 0; MIDIObjectGetIntegerProperty(d, kMIDIPropertyOffline, &offline);
        CFStringRef ds = NULL; drv[0] = 0;
        if (MIDIObjectGetStringProperty(d, kMIDIPropertyDriverOwner, &ds) == noErr && ds) {
            CFStringGetCString(ds, drv, sizeof drv, kCFStringEncodingUTF8); CFRelease(ds);
        }
        midi_name(d, name, sizeof name);
        printf("  device \"%s\" driver=%s %s\n", name, drv[0] ? drv : "-", offline ? "(offline)" : "(online)");
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) { midi_name(MIDIGetSource(i), name, sizeof name); printf("  source \"%s\"\n", name); }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) { midi_name(MIDIGetDestination(i), name, sizeof name); printf("  destination \"%s\"\n", name); }
    return 0;
}

static _Atomic int gMidiRx;
static void coremidi_read(const MIDIPacketList *pl, void *r, void *s)
{
    (void)r; (void)s;
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++) {
        printf("  rx:"); for (UInt16 k = 0; k < p->length; k++) printf(" %02X", p->data[k]); printf("\n");
        atomic_fetch_add(&gMidiRx, 1);
        p = MIDIPacketNext(p);
    }
}

static bool is_ours(MIDIEndpointRef e)
{
    char name[256]; midi_name(e, name, sizeof name);
    return strstr(name, "Audiophile") != NULL;
}

static int cmd_coremidi_test(double seconds)
{
    MIDIClientRef cl; MIDIPortRef outp, inp;
    if (MIDIClientCreate(CFSTR("apusb-diag"), NULL, NULL, &cl)) { fprintf(stderr, "MIDIClientCreate failed\n"); return 1; }
    MIDIOutputPortCreate(cl, CFSTR("out"), &outp);
    MIDIInputPortCreate(cl, CFSTR("in"), coremidi_read, NULL, &inp);
    MIDIEndpointRef dst = 0, src = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) if (is_ours(MIDIGetDestination(i))) { dst = MIDIGetDestination(i); break; }
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) if (is_ours(MIDIGetSource(i))) { src = MIDIGetSource(i); break; }
    printf("Audiophile MIDI source: %s, destination: %s\n", src ? "found" : "MISSING", dst ? "found" : "MISSING");
    if (src) MIDIPortConnectSource(inp, src, NULL);
    int rc = (src && dst) ? 0 : 1;
    if (dst) {
        /* middle C on/off at low velocity, then active sensing; harmless on any synth */
        Byte buf[256]; MIDIPacketList *pl = (MIDIPacketList *)buf; MIDIPacket *p = MIDIPacketListInit(pl);
        Byte on[3] = { 0x90, 60, 32 }, off[3] = { 0x80, 60, 0 };
        p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, on);
        p = MIDIPacketListAdd(pl, sizeof buf, p, 0, 3, off);
        OSStatus e = MIDISend(outp, dst, pl);
        printf("MIDISend note on/off -> %s (%d)\n", e ? "FAILED" : "ok", (int)e);
        if (e) rc = 1;
    }
    printf("listening %.0f s for incoming MIDI (MIDI OUT->IN cable would echo the note)...\n", seconds);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    printf("received %d packet(s)\n", atomic_load(&gMidiRx));
    MIDIClientDispose(cl);
    return rc;
}

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];
    stream_opts o = { .mode = apusb_mode_default()->name, .seconds = 5.0,
                      .buffer = 64, .level = -30.0, .channel = -1, .xfers = APUSB_STREAM_DEFAULT_XFERS, .framesPerXfer = APUSB_STREAM_DEFAULT_FRAMES_PER_XFER };
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
        else if (!strcmp(a, "--uid") && v) { o.uid = v; i++; }
        else if (!strcmp(a, "--buffer") && v) { o.buffer = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--level") && v) { o.level = atof(v); i++; }
        else if (!strcmp(a, "--loopback")) o.loopback = true;
        else if (!strcmp(a, "--sweep")) o.sweep = true;
        else if (!strcmp(a, "--method") && v) { o.method = atoi(v); i++; }
        else if (!strcmp(a, "--channel") && v) { o.channel = atoi(v); i++; }
        else if (!strcmp(a, "--capture")) o.capture = true;
        else if (!strcmp(a, "--xfers") && v) { o.xfers = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--frames-per-xfer") && v) { o.framesPerXfer = (unsigned)atoi(v); i++; }
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
    if (!strcmp(cmd, "reset")) return cmd_reset(o.method);
    if (!strcmp(cmd, "hal-list")) return cmd_hal_list();
    if (!strcmp(cmd, "coremidi-list")) return cmd_coremidi_list();
    if (!strcmp(cmd, "coremidi-test")) return cmd_coremidi_test(o.seconds);
    if (!strcmp(cmd, "hal-test")) return cmd_hal_test(o.uid, o.buffer, o.seconds, o.loopback, o.level, o.rate, o.channel, o.sweep, o.out);
    if (!strcmp(cmd, "set-mode")) { if (argc < 3) { usage(); return 2; } return cmd_set_mode(argv[2]); }
    usage();
    return 2;
}
