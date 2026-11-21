/*
 * AudiophileUSBDriver.c - experimental Core Audio HAL (AudioServerPlugIn)
 * driver for the M-Audio Audiophile USB (USB ID 0763:2003).
 *
 * The plug-in runs inside coreaudiod and talks to the device from user
 * space through IOUSBLib: no kernel extension, no DriverKit entitlement.
 *
 * Object graph:
 *   PlugIn (1) -> Device (2) -> Input stream (3), Output stream (4)
 *
 * Timing: the device clock exposed to the HAL is derived from the host
 * clock (mach_absolute_time). The playback endpoint is adaptive, so the
 * USB side follows the host clock by varying packet sizes by +/-1 sample;
 * the capture side drops / repeats single frames if it drifts.
 *
 * EXPERIMENTAL - UNSIGNED - UNTESTED ON HARDWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <notify.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "apusb_convert.h"
#include "apusb_modes.h"
#include "apusb_ring.h"
#include "apusb_stream.h"
#include "apusb_usb.h"
#include "apusb_reset.h"

/* ------------------------------------------------------------------------ */
#pragma mark Constants

enum {
    kObjectID_PlugIn       = kAudioObjectPlugInObject,
    kObjectID_Device       = 2,
    kObjectID_Stream_Input = 3,
    kObjectID_Stream_Output = 4,
};

#define kPlugIn_BundleID     "com.harmoniqpunk.audiophileusb.driver"
#define kDevice_Name         "Audiophile USB"
#define kDevice_Manufacturer "M-Audio"
#define kDevice_UID          "AudiophileUSB_0763_2003"
#define kDevice_ModelUID     "AudiophileUSB_Model"

#define kRingFrames          16384u        /* also the zero-timestamp period */
#define kChannels            APUSB_CHANNELS
/* Low-latency USB scheduling: kXfers transfers of kFramesPerXfer 1-ms USB frames. */
#define kXfers               APUSB_STREAM_DEFAULT_XFERS
#define kFramesPerXfer       APUSB_STREAM_DEFAULT_FRAMES_PER_XFER
/* Output: the USB thread reads up to one transfer ahead of "now" -> cover it + 1 ms jitter. */
#define kSafetyOffsetOutMs   (kFramesPerXfer + 1u)
/* Input: data arrives once per transfer -> cover one transfer + 1 ms jitter. */
#define kSafetyOffsetInMs    (kFramesPerXfer + 1u)
#define kCorrectionToleranceMs 2u
#define kResyncThresholdMs   100u

/* Custom property: current mode name (CFString), settable. */
#define kAudiophilePropertyMode  'apmd'
/* Custom property: list of mode names (CFArray of CFString). */
#define kAudiophilePropertyModes 'apms'

enum { kChange_SampleRate = 1, kChange_Mode = 2 };

static os_log_t gLog;
#define LOG(fmt, ...)  os_log(gLog, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) os_log_error(gLog, fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------------ */
#pragma mark State

static pthread_mutex_t          gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static AudioServerPlugInHostRef gHost = NULL;
static _Atomic UInt32           gRefCount = 0;

static const apusb_mode *gMode = NULL;
static Float64           gSampleRate = 48000.0;
static const apusb_mode *gPendingMode = NULL;
static Float64           gPendingRate = 0;
static _Atomic bool      gDevicePresent = false;

/* IO / clock */
static UInt32  gIORunning = 0;
static Float64 gHostTicksPerFrame = 0;
static UInt64  gAnchorHostTime = 0;
static UInt64  gNumberTimeStamps = 0;

static apusb_ring gOutRing, gInRing;

/* USB */
static apusb_device    gUSB;
static apusb_interface gIntf[APUSB_IF_STREAM_LAST + 1];
static apusb_stream   *gPlayStream = NULL, *gCapStream = NULL;
static apusb_pacer     gPacer;
static int64_t         gOutReadPos = 0, gInWritePos = 0;
static bool            gInSynced = false;
static float           gLastInFrame[kChannels];
static float           gInPeak = 0;
static float           gScratchOut[256 * kChannels];
static float           gScratchIn[256 * kChannels];

/* Hot-plug */
static IONotificationPortRef gNotifyPort = NULL;
static io_iterator_t         gAddedIter = 0, gRemovedIter = 0;

/* ------------------------------------------------------------------------ */
#pragma mark Helpers

static Float64 host_ticks_per_second(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
}

static int64_t host_now_sample(void)
{
    if (gHostTicksPerFrame <= 0) return 0;
    return (int64_t)((Float64)(mach_absolute_time() - gAnchorHostTime) / gHostTicksPerFrame);
}

static UInt32 ms_frames(UInt32 ms) { return (UInt32)(gSampleRate * ms / 1000.0); }

static bool mode_has_output(void) { return gMode && gMode->outInterface >= 0; }
static bool mode_has_input(void)  { return gMode && gMode->inInterface >= 0; }

static void notify_properties(AudioObjectID obj, UInt32 count, const AudioObjectPropertyAddress *addrs)
{
    if (gHost) gHost->PropertiesChanged(gHost, obj, count, addrs);
}

static void load_saved_mode(void)
{
    gMode = apusb_mode_default();
    gSampleRate = gMode->defaultRate;
    if (gHost == NULL) return;
    CFPropertyListRef saved = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("Mode"), &saved) == kAudioHardwareNoError && saved) {
        if (CFGetTypeID(saved) == CFStringGetTypeID()) {
            char name[64];
            if (CFStringGetCString((CFStringRef)saved, name, sizeof name, kCFStringEncodingUTF8)) {
                const apusb_mode *m = apusb_mode_by_name(name);
                if (m) { gMode = m; gSampleRate = m->defaultRate; }
            }
        }
        CFRelease(saved);
    }
    saved = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("SampleRate"), &saved) == kAudioHardwareNoError && saved) {
        if (CFGetTypeID(saved) == CFNumberGetTypeID()) {
            Float64 r = 0;
            CFNumberGetValue((CFNumberRef)saved, kCFNumberFloat64Type, &r);
            if (apusb_mode_supports_rate(gMode, r)) gSampleRate = r;
        }
        CFRelease(saved);
    }
}

static void save_settings(void)
{
    if (gHost == NULL) return;
    CFStringRef name = CFStringCreateWithCString(NULL, gMode->name, kCFStringEncodingUTF8);
    gHost->WriteToStorage(gHost, CFSTR("Mode"), name);
    CFRelease(name);
    CFNumberRef rate = CFNumberCreate(NULL, kCFNumberFloat64Type, &gSampleRate);
    gHost->WriteToStorage(gHost, CFSTR("SampleRate"), rate);
    CFRelease(rate);
}

static void fill_asbd(AudioStreamBasicDescription *d, Float64 rate)
{
    memset(d, 0, sizeof *d);
    d->mSampleRate = rate;
    d->mFormatID = kAudioFormatLinearPCM;
    d->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d->mBytesPerPacket = 4 * kChannels;
    d->mFramesPerPacket = 1;
    d->mBytesPerFrame = 4 * kChannels;
    d->mChannelsPerFrame = kChannels;
    d->mBitsPerChannel = 32;
}

/* Formats offered in Audio MIDI Setup. Each maps onto one device mode:
 * 16-bit -> 16bit-48k, 24-bit <= 48 kHz -> 24bit-48k, 24-bit 88.2/96 kHz ->
 * the half-duplex 96k mode of that direction. */
static const Float64 kAllRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
#define kNumAllRates 4u
static const struct { UInt32 bits; Float64 rate; } kPhysFormats[] = {
    { 16, 44100.0 }, { 16, 48000.0 }, { 24, 44100.0 }, { 24, 48000.0 }, { 24, 88200.0 }, { 24, 96000.0 },
};
#define kNumPhysFormats 6u

static UInt32 mode_bits(const apusb_mode *m) { return 8u * apusb_format_bytes(m->format); }

static void fill_phys(AudioStreamBasicDescription *d, Float64 rate, UInt32 bits, bool bigEndian)
{
    memset(d, 0, sizeof *d);
    d->mSampleRate = rate;
    d->mFormatID = kAudioFormatLinearPCM;
    d->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | (bigEndian ? kAudioFormatFlagIsBigEndian : 0);
    d->mBytesPerFrame = d->mBytesPerPacket = (bits / 8) * kChannels;
    d->mFramesPerPacket = 1;
    d->mChannelsPerFrame = kChannels;
    d->mBitsPerChannel = bits;
}

/* dir: 0 = output stream, 1 = input stream, -1 = device-wide request. bits 0 = keep depth. */
static const apusb_mode *mode_for(UInt32 bits, Float64 rate, int dir)
{
    if (rate > 48000.0) {
        if (bits == 16) return NULL;
        if (gMode && mode_bits(gMode) == 24 && apusb_mode_supports_rate(gMode, rate) && dir == -1) return gMode;
        return apusb_mode_by_name(dir == 1 ? "24bit-96k-record" : "24bit-96k-play");
    }
    if (gMode && (bits == 0 || mode_bits(gMode) == bits) && apusb_mode_supports_rate(gMode, rate)) return gMode;
    if (bits == 0) bits = (gMode && gMode->defaultRate <= 48000.0) ? mode_bits(gMode) : 24;
    return apusb_mode_by_name(bits == 16 ? "16bit-48k" : "24bit-48k");
}

/* ------------------------------------------------------------------------ */
#pragma mark USB streaming callbacks

static unsigned play_packet_frames(void *ctx)
{
    (void)ctx;
    int64_t err = host_now_sample() - gOutReadPos;
    int64_t tol = ms_frames(kCorrectionToleranceMs);
    if (err > (int64_t)ms_frames(kResyncThresholdMs) || err < -(int64_t)ms_frames(kResyncThresholdMs)) {
        gOutReadPos = host_now_sample();
        err = 0;
    }
    int corr = err > tol ? 1 : (err < -tol ? -1 : 0);
    return apusb_pacer_next(&gPacer, corr);
}

static void play_fill(void *ctx, uint8_t *dst, unsigned frames)
{
    (void)ctx;
    while (frames > 0) {
        unsigned n = frames > 256 ? 256 : frames;
        apusb_ring_read(&gOutRing, gOutReadPos, gScratchOut, n, true);
        apusb_float_to_pcm(gScratchOut, dst, n, kChannels, gMode->format);
        dst += (size_t)n * kChannels * apusb_format_bytes(gMode->format);
        gOutReadPos += n;
        frames -= n;
    }
}

static void cap_consume(void *ctx, const uint8_t *src, unsigned frames)
{
    (void)ctx;
    int64_t now = host_now_sample();
    if (!gInSynced || llabs(now - gInWritePos) > (long long)ms_frames(kResyncThresholdMs)) {
        gInWritePos = now;
        gInSynced = true;
    }
    int64_t tol = ms_frames(kCorrectionToleranceMs);
    unsigned bpf = kChannels * apusb_format_bytes(gMode->format);
    if (now - gInWritePos > tol) {
        /* device slower than host clock: repeat one frame */
        apusb_ring_write(&gInRing, gInWritePos++, gLastInFrame, 1);
    } else if (gInWritePos - now > tol && frames > 0) {
        /* device faster: drop one frame */
        src += bpf;
        frames--;
    }
    while (frames > 0) {
        unsigned n = frames > 256 ? 256 : frames;
        apusb_pcm_to_float(src, gScratchIn, n, kChannels, gMode->format);
        for (unsigned k = 0; k < n * kChannels; k++) { float v = fabsf(gScratchIn[k]); if (v > gInPeak) gInPeak = v; }
        apusb_ring_write(&gInRing, gInWritePos, gScratchIn, n);
        memcpy(gLastInFrame, gScratchIn + (size_t)(n - 1) * kChannels, sizeof gLastInFrame);
        gInWritePos += n;
        src += (size_t)n * bpf;
        frames -= n;
    }
}

static void stream_status(void *ctx, IOReturn r, const char *what)
{
    LOGE("AudiophileUSB: %{public}s (%{public}s) on %{public}s", what, apusb_strerror(r),
         ctx ? "capture" : "playback");
}

/* ------------------------------------------------------------------------ */
#pragma mark USB open / close

static void log_stream_stats(apusb_stream *st, const char *what)
{
    uint64_t pk = 0, er = 0, rs = 0;
    apusb_stream_stats(st, &pk, &er, &rs);
    LOG("AudiophileUSB: %{public}s stopped: %llu USB packets, %llu errors, %llu resyncs", what,
        (unsigned long long)pk, (unsigned long long)er, (unsigned long long)rs);
}

/* The Audiophile USB keeps internal state from the last configuration it
 * streamed in; switching bit depth / rate without a USB re-enumeration gives
 * garbage samples (measured: silent input at -5 dBFS RMS). So whenever the
 * configuration differs from the one used since the last reset, ask
 * audiophileusbd (root) to re-enumerate the device first (~1-2 s). */
static _Atomic bool gResetting = false;
static double gLastConfigKey = -1;

static bool request_device_reset(void)
{
    int tok = -1;
    uint64_t before = 0, now = 0;
    if (notify_register_check(APUSB_NOTIFY_RESET_DONE, &tok) != NOTIFY_STATUS_OK) {
        LOGE("AudiophileUSB: notify unavailable, cannot reset device");
        return false;
    }
    notify_get_state(tok, &before);
    atomic_store(&gResetting, true);
    notify_post(APUSB_NOTIFY_RESET_REQUEST);
    bool ok = false;
    for (int i = 0; i < 100 && !ok; i++) {          /* <= 5 s */
        usleep(50 * 1000);
        notify_get_state(tok, &now);
        ok = now != before;
    }
    notify_cancel(tok);
    LOG("AudiophileUSB: device reset %{public}s", ok ? "done" : "timed out (is audiophileusbd running?)");
    return ok;
}

static void usb_close(void)
{
    if (gPlayStream) { apusb_stream_stop(gPlayStream); log_stream_stats(gPlayStream, "playback"); apusb_stream_destroy(gPlayStream); gPlayStream = NULL; }
    if (gCapStream)  { apusb_stream_stop(gCapStream); log_stream_stats(gCapStream, "capture");
                       LOG("AudiophileUSB: capture peak %d dBFS (x10)", gInPeak > 0 ? (int)(200.0 * log10(gInPeak)) : -2000);
                       gInPeak = 0; apusb_stream_destroy(gCapStream);  gCapStream = NULL; }
    for (int i = APUSB_IF_STREAM_FIRST; i <= APUSB_IF_STREAM_LAST; i++) {
        if (gIntf[i].intf) {
            apusb_interface_set_alt(&gIntf[i], 0);
            apusb_interface_close(&gIntf[i]);
        }
    }
    if (gUSB.service) apusb_device_release(&gUSB);
}

static OSStatus usb_open_and_start(void)
{
    double key = (double)(gMode - apusb_mode_at(0)) * 1.0e6 + gSampleRate;
    if (key != gLastConfigKey) {
        if (request_device_reset()) gLastConfigKey = key;
    }
    io_service_t svc = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
    atomic_store(&gResetting, false);
    if (!svc) { LOGE("AudiophileUSB: device not found"); return kAudioHardwareBadDeviceError; }
    IOReturn kr = apusb_device_attach(svc, &gUSB);
    IOObjectRelease(svc);
    if (kr) { LOGE("AudiophileUSB: device_create %{public}s", apusb_strerror(kr)); return kAudioHardwareUnspecifiedError; }

    kr = apusb_device_ensure_configured(&gUSB);
    if (kr) LOGE("AudiophileUSB: ensure_configured %{public}s (continuing)", apusb_strerror(kr));

    /* Documented requirement: put every streaming interface into alt 0
     * before selecting the operating alternate setting. */
    for (int i = APUSB_IF_STREAM_FIRST; i <= APUSB_IF_STREAM_LAST; i++) {
        kr = apusb_interface_open(&gUSB, (UInt8)i, false, &gIntf[i]);
        if (kr) {
            LOGE("AudiophileUSB: open interface %d: %{public}s", i, apusb_strerror(kr));
            continue;
        }
        apusb_interface_set_alt(&gIntf[i], 0);
    }

    unsigned bpf = kChannels * apusb_format_bytes(gMode->format);
    apusb_stream_callbacks cb = { 0 };
    cb.status = stream_status;

    if (mode_has_output()) {
        apusb_interface *it = &gIntf[gMode->outInterface];
        apusb_pipe pipe;
        if (it->intf == NULL) goto fail;
        if ((kr = apusb_interface_set_alt(it, gMode->altSetting))) goto fail_kr;
        if ((kr = apusb_interface_find_pipe(it, kUSBOut, kUSBIsoc, &pipe))) goto fail_kr;
        kr = apusb_set_sample_rate(it, pipe.address, (UInt32)gSampleRate);
        if (kr) LOGE("AudiophileUSB: set rate (out) %{public}s", apusb_strerror(kr));
        apusb_pacer_init(&gPacer, (uint32_t)gSampleRate);
        gOutReadPos = host_now_sample();
        cb.packet_frames = play_packet_frames;
        cb.fill = play_fill;
        cb.consume = NULL;
        cb.ctx = NULL;
        gPlayStream = apusb_stream_create(it, &pipe, false, bpf, kXfers, kFramesPerXfer, &cb);
        if (gPlayStream == NULL) goto fail;
        if ((kr = apusb_stream_start(gPlayStream))) goto fail_kr;
    }
    if (mode_has_input()) {
        apusb_interface *it = &gIntf[gMode->inInterface];
        apusb_pipe pipe;
        if (it->intf == NULL) goto fail;
        if ((kr = apusb_interface_set_alt(it, gMode->altSetting))) goto fail_kr;
        if ((kr = apusb_interface_find_pipe(it, kUSBIn, kUSBIsoc, &pipe))) goto fail_kr;
        kr = apusb_set_sample_rate(it, pipe.address, (UInt32)gSampleRate);
        if (kr) LOGE("AudiophileUSB: set rate (in) %{public}s", apusb_strerror(kr));
        gInSynced = false;
        cb.packet_frames = NULL;
        cb.fill = NULL;
        cb.consume = cap_consume;
        cb.ctx = (void *)1;
        gCapStream = apusb_stream_create(it, &pipe, true, bpf, kXfers, kFramesPerXfer, &cb);
        if (gCapStream == NULL) goto fail;
        if ((kr = apusb_stream_start(gCapStream))) goto fail_kr;
    }
    LOG("AudiophileUSB: started mode %{public}s at %d Hz", gMode->name, (int)gSampleRate);
    return kAudioHardwareNoError;

fail_kr:
    LOGE("AudiophileUSB: start failed: %{public}s", apusb_strerror(kr));
fail:
    usb_close();
    return kAudioHardwareUnspecifiedError;
}

/* ------------------------------------------------------------------------ */
#pragma mark Hot-plug

static void device_list_changed(void)
{
    AudioObjectPropertyAddress addrs[] = {
        { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
    };
    notify_properties(kObjectID_PlugIn, 2, addrs);
}

static void drain(io_iterator_t it)
{
    io_service_t s;
    while ((s = IOIteratorNext(it)) != 0) IOObjectRelease(s);
}

/* AppleUSBAudio (built into macOS) claims the AudioStreaming interfaces 1-5
 * exclusively. When it does, user space cannot stream to the device, so we must
 * not publish a second, dead "Audiophile USB" device next to Apple's ones. */
static bool registry_has_apple_audio(io_registry_entry_t e, int depth)
{
    if (depth > 4) return false;
    io_iterator_t it = 0;
    bool found = false;
    if (IORegistryEntryGetChildIterator(e, kIOServicePlane, &it) != KERN_SUCCESS) return false;
    io_registry_entry_t c;
    while (!found && (c = IOIteratorNext(it)) != 0) {
        io_name_t cls;
        if (IOObjectGetClass(c, cls) == KERN_SUCCESS && strncmp(cls, "AppleUSBAudio", 13) == 0) found = true;
        else found = registry_has_apple_audio(c, depth + 1);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
    return found;
}

static bool device_usable(void)
{
    io_service_t s = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
    if (!s) return false;
    bool native = registry_has_apple_audio(s, 0);
    IOObjectRelease(s);
    return !native;
}

static void evaluate_presence(void *ctx)
{
    (void)ctx;
    if (atomic_load(&gResetting)) return;
    bool usable = device_usable();
    if (atomic_exchange(&gDevicePresent, usable) != usable) {
        LOG("AudiophileUSB: device %{public}s", usable ? "available (interfaces free)" :
            "not available (absent, or macOS AppleUSBAudio owns it - is audiophileusbd running?)");
        if (!usable) {
            AudioObjectPropertyAddress alive = { kAudioDevicePropertyDeviceIsAlive,
                kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            notify_properties(kObjectID_Device, 1, &alive);
        }
        device_list_changed();
    }
}

static dispatch_source_t gPresenceTimer;
static void presence_tick(void *ctx) { (void)ctx; if (gIORunning == 0) evaluate_presence(NULL); }

static void on_added(void *refcon, io_iterator_t it)
{
    (void)refcon;
    drain(it);
    /* give the kernel a moment to match (or not) AppleUSBAudio first */
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC),
                     dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL, evaluate_presence);
}

static void on_removed(void *refcon, io_iterator_t it)
{
    (void)refcon;
    drain(it);
    evaluate_presence(NULL);
}

static CFMutableDictionaryRef usb_matching(void)
{
    return apusb_matching(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
}

static void start_hotplug(void)
{
    gNotifyPort = IONotificationPortCreate(kIOMasterPortDefault);
    if (gNotifyPort == NULL) return;
    IONotificationPortSetDispatchQueue(gNotifyPort,
        dispatch_queue_create("com.harmoniqpunk.audiophileusb.hotplug", DISPATCH_QUEUE_SERIAL));
    if (IOServiceAddMatchingNotification(gNotifyPort, kIOFirstMatchNotification, usb_matching(),
                                         on_added, NULL, &gAddedIter) == kIOReturnSuccess) {
        drain(gAddedIter);
        atomic_store(&gDevicePresent, device_usable());
        LOG("AudiophileUSB: device %{public}s at start", atomic_load(&gDevicePresent) ? "available" : "not available");
    }
    if (IOServiceAddMatchingNotification(gNotifyPort, kIOTerminatedNotification, usb_matching(),
                                         on_removed, NULL, &gRemovedIter) == kIOReturnSuccess)
        drain(gRemovedIter);
    /* audiophileusbd detaches AppleUSBAudio without a new plug event: re-check
     * ownership every 2 s while IO is not running (cheap registry walk). */
    gPresenceTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(gPresenceTimer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler_f(gPresenceTimer, presence_tick);
    dispatch_resume(gPresenceTimer);
}

/* ------------------------------------------------------------------------ */
#pragma mark Interface prototypes

static HRESULT  APUSB_QueryInterface(void *, REFIID, LPVOID *);
static ULONG    APUSB_AddRef(void *);
static ULONG    APUSB_Release(void *);
static OSStatus APUSB_Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
static OSStatus APUSB_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus APUSB_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus APUSB_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus APUSB_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus APUSB_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus APUSB_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean  APUSB_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus APUSB_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus APUSB_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus APUSB_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus APUSB_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus APUSB_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus APUSB_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus APUSB_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus APUSB_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus APUSB_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus APUSB_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus APUSB_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    APUSB_QueryInterface, APUSB_AddRef, APUSB_Release,
    APUSB_Initialize, APUSB_CreateDevice, APUSB_DestroyDevice,
    APUSB_AddDeviceClient, APUSB_RemoveDeviceClient,
    APUSB_PerformDeviceConfigurationChange, APUSB_AbortDeviceConfigurationChange,
    APUSB_HasProperty, APUSB_IsPropertySettable, APUSB_GetPropertyDataSize,
    APUSB_GetPropertyData, APUSB_SetPropertyData,
    APUSB_StartIO, APUSB_StopIO, APUSB_GetZeroTimeStamp,
    APUSB_WillDoIOOperation, APUSB_BeginIOOperation, APUSB_DoIOOperation, APUSB_EndIOOperation
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef        gDriverRef = &gInterfacePtr;

/* ------------------------------------------------------------------------ */
#pragma mark Factory

__attribute__((visibility("default"))) void *AudiophileUSB_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);
__attribute__((visibility("default"))) void *AudiophileUSB_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID)
{
    (void)allocator;
    if (CFEqual(requestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

static HRESULT APUSB_QueryInterface(void *driver, REFIID iid, LPVOID *out)
{
    if (driver != gDriverRef || out == NULL) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&gRefCount, 1);
        *out = gDriverRef;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}

static ULONG APUSB_AddRef(void *driver)
{
    if (driver != gDriverRef) return 0;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG APUSB_Release(void *driver)
{
    if (driver != gDriverRef) return 0;
    UInt32 c = atomic_load(&gRefCount);
    if (c > 0) c = atomic_fetch_sub(&gRefCount, 1) - 1;
    return c;
}

static OSStatus APUSB_Initialize(AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef host)
{
    if (driver != gDriverRef) return kAudioHardwareBadObjectError;
    gLog = os_log_create(kPlugIn_BundleID, "driver");
    gHost = host;
    pthread_mutex_lock(&gStateMutex);
    load_saved_mode();
    pthread_mutex_unlock(&gStateMutex);
    if (!apusb_ring_init(&gOutRing, kRingFrames, kChannels) || !apusb_ring_init(&gInRing, kRingFrames, kChannels))
        return kAudioHardwareUnspecifiedError;
    start_hotplug();
    LOG("AudiophileUSB: initialized, mode %{public}s, device %{public}s", gMode->name,
        atomic_load(&gDevicePresent) ? "present" : "absent");
    return kAudioHardwareNoError;
}

static OSStatus APUSB_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out)
{ (void)d; (void)desc; (void)ci; (void)out; return kAudioHardwareUnsupportedOperationError; }
static OSStatus APUSB_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{ (void)d; (void)id; return kAudioHardwareUnsupportedOperationError; }
static OSStatus APUSB_AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci)
{ (void)ci; return (d == gDriverRef && id == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError; }
static OSStatus APUSB_RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID id, const AudioServerPlugInClientInfo *ci)
{ (void)ci; return (d == gDriverRef && id == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError; }

static OSStatus APUSB_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info)
{
    (void)info;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (action == kChange_SampleRate && gPendingRate > 0) {
        gSampleRate = gPendingRate;
    } else if (action == kChange_Mode && gPendingMode) {
        gMode = gPendingMode;
        if (gPendingRate > 0 && apusb_mode_supports_rate(gMode, gPendingRate)) gSampleRate = gPendingRate;
        else if (!apusb_mode_supports_rate(gMode, gSampleRate)) gSampleRate = gMode->defaultRate;
        LOG("AudiophileUSB: mode %{public}s, %d Hz", gMode->name, (int)gSampleRate);
    }
    gPendingRate = 0;
    gPendingMode = NULL;
    gHostTicksPerFrame = host_ticks_per_second() / gSampleRate;
    save_settings();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus APUSB_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID id, UInt64 action, void *info)
{
    (void)action; (void)info;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    gPendingRate = 0;
    gPendingMode = NULL;
    return kAudioHardwareNoError;
}

/* ------------------------------------------------------------------------ */
#pragma mark Properties

static bool is_stream(AudioObjectID id) { return id == kObjectID_Stream_Input || id == kObjectID_Stream_Output; }

static Boolean APUSB_HasProperty(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a)
{
    (void)pid;
    if (d != gDriverRef || a == NULL) return false;
    switch (id) {
    case kObjectID_PlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        }
        return false;
    case kObjectID_Device:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices: case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
        case kAudioObjectPropertyControlList: case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyStreams:
            return true;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo:
            return a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput;
        case kAudiophilePropertyMode: case kAudiophilePropertyModes:
            return true;
        }
        return false;
    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects: case kAudioStreamPropertyIsActive: case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        }
        return false;
    }
    return false;
}

static OSStatus APUSB_IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
    if (!APUSB_HasProperty(d, id, pid, a) || out == NULL) return kAudioHardwareUnknownPropertyError;
    *out = false;
    if (id == kObjectID_Device &&
        (a->mSelector == kAudioDevicePropertyNominalSampleRate || a->mSelector == kAudiophilePropertyMode))
        *out = true;
    if (is_stream(id) &&
        (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat))
        *out = true;
    return kAudioHardwareNoError;
}

static UInt32 owned_device_count(void) { return atomic_load(&gDevicePresent) ? 1u : 0u; }

static OSStatus APUSB_GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *out)
{
    (void)qs; (void)q;
    if (!APUSB_HasProperty(d, id, pid, a) || out == NULL) return kAudioHardwareUnknownPropertyError;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
        *out = sizeof(AudioClassID); return 0;
    case kAudioObjectPropertyOwner: case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioDevicePropertyClockDomain:
        *out = sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID: case kAudioPlugInPropertyResourceBundle:
    case kAudiophilePropertyMode: case kAudiophilePropertyModes:
        *out = sizeof(CFTypeRef); return 0;
    case kAudioObjectPropertyCustomPropertyInfoList:
        *out = 2 * sizeof(AudioServerPlugInCustomPropertyInfo); return 0;
    case kAudioObjectPropertyControlList:
        *out = 0; return 0;
    case kAudioObjectPropertyOwnedObjects:
        if (id == kObjectID_PlugIn) *out = owned_device_count() * sizeof(AudioObjectID);
        else if (id == kObjectID_Device) {
            UInt32 n = 0;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeInput) n++;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput) n++;
            *out = n * sizeof(AudioObjectID);
        } else *out = 0;
        return 0;
    case kAudioPlugInPropertyDeviceList:
        *out = owned_device_count() * sizeof(AudioObjectID); return 0;
    case kAudioDevicePropertyStreams: {
        UInt32 n = 0;
        if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeInput) n++;
        if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput) n++;
        *out = n * sizeof(AudioObjectID); return 0;
    }
    case kAudioDevicePropertyRelatedDevices:
        *out = sizeof(AudioObjectID); return 0;
    case kAudioDevicePropertyNominalSampleRate:
        *out = sizeof(Float64); return 0;
    case kAudioDevicePropertyAvailableNominalSampleRates:
        *out = kNumAllRates * sizeof(AudioValueRange); return 0;
    case kAudioDevicePropertyPreferredChannelsForStereo:
        *out = 2 * sizeof(UInt32); return 0;
    case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
        *out = sizeof(AudioStreamBasicDescription); return 0;
    case kAudioStreamPropertyAvailableVirtualFormats:
        *out = kNumAllRates * sizeof(AudioStreamRangedDescription); return 0;
    case kAudioStreamPropertyAvailablePhysicalFormats:
        *out = kNumPhysFormats * sizeof(AudioStreamRangedDescription); return 0;
    default:
        *out = sizeof(UInt32); return 0;
    }
}

#define NEED(sz) do { if (inDataSize < (sz)) return kAudioHardwareBadPropertySizeError; *outDataSize = (sz); } while (0)

static OSStatus APUSB_GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                      UInt32 qs, const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *out)
{
    if (!APUSB_HasProperty(d, id, pid, a) || outDataSize == NULL || out == NULL) return kAudioHardwareUnknownPropertyError;
    OSStatus err = kAudioHardwareNoError;
    pthread_mutex_lock(&gStateMutex);

    if (id == kObjectID_PlugIn) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *(AudioClassID *)out = kAudioObjectClassID; break;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *(AudioClassID *)out = kAudioPlugInClassID; break;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *(AudioObjectID *)out = kAudioObjectUnknown; break;
        case kAudioObjectPropertyManufacturer: NEED(sizeof(CFStringRef)); *(CFStringRef *)out = CFSTR("harmoniqpunk (community)"); break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            UInt32 n = owned_device_count();
            if (inDataSize < n * sizeof(AudioObjectID)) n = inDataSize / sizeof(AudioObjectID);
            if (n) ((AudioObjectID *)out)[0] = kObjectID_Device;
            *outDataSize = n * sizeof(AudioObjectID);
            break;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            NEED(sizeof(AudioObjectID));
            AudioObjectID r = kAudioObjectUnknown;
            if (qs == sizeof(CFStringRef) && q && *(CFStringRef const *)q &&
                CFStringCompare(*(CFStringRef const *)q, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo &&
                owned_device_count())
                r = kObjectID_Device;
            *(AudioObjectID *)out = r;
            break;
        }
        case kAudioPlugInPropertyResourceBundle: NEED(sizeof(CFStringRef)); *(CFStringRef *)out = CFSTR(""); break;
        default: err = kAudioHardwareUnknownPropertyError;
        }
    } else if (id == kObjectID_Device) {
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *(AudioClassID *)out = kAudioObjectClassID; break;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *(AudioClassID *)out = kAudioDeviceClassID; break;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *(AudioObjectID *)out = kObjectID_PlugIn; break;
        case kAudioObjectPropertyName: NEED(sizeof(CFStringRef)); *(CFStringRef *)out = CFSTR(kDevice_Name); break;
        case kAudioObjectPropertyManufacturer: NEED(sizeof(CFStringRef)); *(CFStringRef *)out = CFSTR(kDevice_Manufacturer); break;
        case kAudioDevicePropertyDeviceUID: NEED(sizeof(CFStringRef)); *(CFStringRef *)out = CFSTR(kDevice_UID); break;
        case kAudioDevicePropertyModelUID: NEED(sizeof(CFStringRef)); *(CFStringRef *)out = CFSTR(kDevice_ModelUID); break;
        case kAudioDevicePropertyTransportType: NEED(sizeof(UInt32)); *(UInt32 *)out = kAudioDeviceTransportTypeUSB; break;
        case kAudioDevicePropertyRelatedDevices: NEED(sizeof(AudioObjectID)); *(AudioObjectID *)out = kObjectID_Device; break;
        case kAudioDevicePropertyClockDomain: NEED(sizeof(UInt32)); *(UInt32 *)out = 0; break;
        case kAudioDevicePropertyDeviceIsAlive: NEED(sizeof(UInt32)); *(UInt32 *)out = atomic_load(&gDevicePresent) ? 1 : 0; break;
        case kAudioDevicePropertyDeviceIsRunning: NEED(sizeof(UInt32)); *(UInt32 *)out = gIORunning > 0 ? 1 : 0; break;
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            NEED(sizeof(UInt32));
            *(UInt32 *)out = (a->mScope == kAudioObjectPropertyScopeInput) ? mode_has_input() : mode_has_output();
            break;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            NEED(sizeof(UInt32));
            *(UInt32 *)out = (a->mScope == kAudioObjectPropertyScopeOutput) ? mode_has_output() : 0;
            break;
        case kAudioDevicePropertyLatency:
            NEED(sizeof(UInt32));
            /* output: data queued in in-flight USB transfers; input: one transfer */
            *(UInt32 *)out = (a->mScope == kAudioObjectPropertyScopeOutput) ? ms_frames((kXfers - 1) * kFramesPerXfer) : ms_frames(kFramesPerXfer);
            break;
        case kAudioDevicePropertySafetyOffset:
            NEED(sizeof(UInt32));
            *(UInt32 *)out = (a->mScope == kAudioObjectPropertyScopeInput) ? ms_frames(kSafetyOffsetInMs) : ms_frames(kSafetyOffsetOutMs);
            break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID ids[2]; UInt32 n = 0;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeInput) ids[n++] = kObjectID_Stream_Input;
            if (a->mScope == kAudioObjectPropertyScopeGlobal || a->mScope == kAudioObjectPropertyScopeOutput) ids[n++] = kObjectID_Stream_Output;
            if (inDataSize < n * sizeof(AudioObjectID)) n = inDataSize / sizeof(AudioObjectID);
            memcpy(out, ids, n * sizeof(AudioObjectID));
            *outDataSize = n * sizeof(AudioObjectID);
            break;
        }
        case kAudioObjectPropertyControlList: *outDataSize = 0; break;
        case kAudioDevicePropertyNominalSampleRate: NEED(sizeof(Float64)); *(Float64 *)out = gSampleRate; break;
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = kNumAllRates;
            if (inDataSize < n * sizeof(AudioValueRange)) n = inDataSize / sizeof(AudioValueRange);
            for (UInt32 i = 0; i < n; i++) {
                ((AudioValueRange *)out)[i].mMinimum = kAllRates[i];
                ((AudioValueRange *)out)[i].mMaximum = kAllRates[i];
            }
            *outDataSize = n * sizeof(AudioValueRange);
            break;
        }
        case kAudioDevicePropertyIsHidden: NEED(sizeof(UInt32)); *(UInt32 *)out = 0; break;
        case kAudioDevicePropertyPreferredChannelsForStereo:
            NEED(2 * sizeof(UInt32)); ((UInt32 *)out)[0] = 1; ((UInt32 *)out)[1] = 2; break;
        case kAudioDevicePropertyZeroTimeStampPeriod: NEED(sizeof(UInt32)); *(UInt32 *)out = kRingFrames; break;
        case kAudioObjectPropertyCustomPropertyInfoList: {
            NEED(2 * sizeof(AudioServerPlugInCustomPropertyInfo));
            AudioServerPlugInCustomPropertyInfo *info = (AudioServerPlugInCustomPropertyInfo *)out;
            info[0].mSelector = kAudiophilePropertyMode;
            info[0].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFString;
            info[0].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            info[1].mSelector = kAudiophilePropertyModes;
            info[1].mPropertyDataType = kAudioServerPlugInCustomPropertyDataTypeCFPropertyList;
            info[1].mQualifierDataType = kAudioServerPlugInCustomPropertyDataTypeNone;
            break;
        }
        case kAudiophilePropertyMode:
            NEED(sizeof(CFStringRef));
            *(CFStringRef *)out = CFStringCreateWithCString(NULL, gMode->name, kCFStringEncodingUTF8);
            break;
        case kAudiophilePropertyModes: {
            NEED(sizeof(CFArrayRef));
            CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
            for (unsigned i = 0; i < apusb_mode_count(); i++) {
                CFStringRef s = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s: %s"),
                                                         apusb_mode_at(i)->name, apusb_mode_at(i)->description);
                CFArrayAppendValue(arr, s);
                CFRelease(s);
            }
            *(CFArrayRef *)out = arr;
            break;
        }
        default: err = kAudioHardwareUnknownPropertyError;
        }
    } else if (is_stream(id)) {
        bool isIn = (id == kObjectID_Stream_Input);
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: NEED(sizeof(AudioClassID)); *(AudioClassID *)out = kAudioObjectClassID; break;
        case kAudioObjectPropertyClass: NEED(sizeof(AudioClassID)); *(AudioClassID *)out = kAudioStreamClassID; break;
        case kAudioObjectPropertyOwner: NEED(sizeof(AudioObjectID)); *(AudioObjectID *)out = kObjectID_Device; break;
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; break;
        case kAudioStreamPropertyIsActive: NEED(sizeof(UInt32)); *(UInt32 *)out = isIn ? mode_has_input() : mode_has_output(); break;
        case kAudioStreamPropertyDirection: NEED(sizeof(UInt32)); *(UInt32 *)out = isIn ? 1 : 0; break;
        case kAudioStreamPropertyTerminalType:
            NEED(sizeof(UInt32)); *(UInt32 *)out = isIn ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine; break;
        case kAudioStreamPropertyStartingChannel: NEED(sizeof(UInt32)); *(UInt32 *)out = 1; break;
        case kAudioStreamPropertyLatency: NEED(sizeof(UInt32)); *(UInt32 *)out = 0; break;
        case kAudioStreamPropertyVirtualFormat:
            NEED(sizeof(AudioStreamBasicDescription));
            fill_asbd((AudioStreamBasicDescription *)out, gSampleRate);
            break;
        case kAudioStreamPropertyPhysicalFormat:
            NEED(sizeof(AudioStreamBasicDescription));
            fill_phys((AudioStreamBasicDescription *)out, gSampleRate, mode_bits(gMode),
                      gMode->format == APUSB_FMT_S24_3BE || gMode->format == APUSB_FMT_S16BE);
            break;
        case kAudioStreamPropertyAvailableVirtualFormats: {
            UInt32 n = kNumAllRates;
            if (inDataSize < n * sizeof(AudioStreamRangedDescription)) n = inDataSize / sizeof(AudioStreamRangedDescription);
            for (UInt32 i = 0; i < n; i++) {
                AudioStreamRangedDescription *r = &((AudioStreamRangedDescription *)out)[i];
                fill_asbd(&r->mFormat, kAllRates[i]);
                r->mSampleRateRange.mMinimum = r->mSampleRateRange.mMaximum = kAllRates[i];
            }
            *outDataSize = n * sizeof(AudioStreamRangedDescription);
            break;
        }
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = kNumPhysFormats;
            if (inDataSize < n * sizeof(AudioStreamRangedDescription)) n = inDataSize / sizeof(AudioStreamRangedDescription);
            for (UInt32 i = 0; i < n; i++) {
                AudioStreamRangedDescription *r = &((AudioStreamRangedDescription *)out)[i];
                fill_phys(&r->mFormat, kPhysFormats[i].rate, kPhysFormats[i].bits, kPhysFormats[i].bits == 24);
                r->mSampleRateRange.mMinimum = r->mSampleRateRange.mMaximum = kPhysFormats[i].rate;
            }
            *outDataSize = n * sizeof(AudioStreamRangedDescription);
            break;
        }
        default: err = kAudioHardwareUnknownPropertyError;
        }
    } else {
        err = kAudioHardwareBadObjectError;
    }
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

static OSStatus request_config(const apusb_mode *m, Float64 rate)
{
    if (m == NULL || !apusb_mode_supports_rate(m, rate)) return kAudioDeviceUnsupportedFormatError;
    if (m == gMode && rate == gSampleRate) return kAudioHardwareNoError;
    gPendingMode = m;
    gPendingRate = rate;
    return gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, kChange_Mode, NULL);
}

static OSStatus request_rate(Float64 rate) { return request_config(mode_for(0, rate, -1), rate); }

static OSStatus APUSB_SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID id, pid_t pid, const AudioObjectPropertyAddress *a,
                                      UInt32 qs, const void *q, UInt32 size, const void *data)
{
    (void)qs; (void)q;
    if (!APUSB_HasProperty(d, id, pid, a) || data == NULL) return kAudioHardwareUnknownPropertyError;
    OSStatus err = kAudioHardwareUnsupportedOperationError;
    pthread_mutex_lock(&gStateMutex);
    if (id == kObjectID_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        err = (size == sizeof(Float64)) ? request_rate(*(const Float64 *)data) : kAudioHardwareBadPropertySizeError;
    } else if (is_stream(id) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (size != sizeof(AudioStreamBasicDescription)) err = kAudioHardwareBadPropertySizeError;
        else {
            const AudioStreamBasicDescription *f = (const AudioStreamBasicDescription *)data;
            int dir = id == kObjectID_Stream_Input ? 1 : 0;
            if (f->mFormatID != kAudioFormatLinearPCM || f->mChannelsPerFrame != kChannels)
                err = kAudioDeviceUnsupportedFormatError;
            else if (a->mSelector == kAudioStreamPropertyVirtualFormat)
                err = f->mBitsPerChannel == 32 ? request_config(mode_for(0, f->mSampleRate, dir), f->mSampleRate)
                                               : kAudioDeviceUnsupportedFormatError;
            else if (f->mBitsPerChannel == 16 || f->mBitsPerChannel == 24)
                err = request_config(mode_for(f->mBitsPerChannel, f->mSampleRate, dir), f->mSampleRate);
            else err = kAudioDeviceUnsupportedFormatError;
        }
    } else if (id == kObjectID_Device && a->mSelector == kAudiophilePropertyMode) {
        if (size != sizeof(CFStringRef)) err = kAudioHardwareBadPropertySizeError;
        else {
            char name[64] = { 0 };
            CFStringRef s = *(CFStringRef const *)data;
            const apusb_mode *m = NULL;
            if (s && CFGetTypeID(s) == CFStringGetTypeID() &&
                CFStringGetCString(s, name, sizeof name, kCFStringEncodingUTF8))
                m = apusb_mode_by_name(name);
            if (m == NULL) err = kAudioHardwareIllegalOperationError;
            else if (m == gMode) err = kAudioHardwareNoError;
            else {
                gPendingMode = m;
                gPendingRate = 0;
                err = gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, kChange_Mode, NULL);
            }
        }
    }
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

/* ------------------------------------------------------------------------ */
#pragma mark IO

static OSStatus APUSB_StartIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)client;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    OSStatus err = kAudioHardwareNoError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning == 0) {
        gHostTicksPerFrame = host_ticks_per_second() / gSampleRate;
        gAnchorHostTime = mach_absolute_time();
        gNumberTimeStamps = 0;
        apusb_ring_clear(&gOutRing);
        apusb_ring_clear(&gInRing);
        err = usb_open_and_start();
    }
    if (err == kAudioHardwareNoError) gIORunning++;
    pthread_mutex_unlock(&gStateMutex);
    return err;
}

static OSStatus APUSB_StopIO(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client)
{
    (void)client;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gIORunning > 0 && --gIORunning == 0) usb_close();
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus APUSB_GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client,
                                       Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
    (void)client;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kRingFrames;
    UInt64 now = mach_absolute_time();
    UInt64 next = gAnchorHostTime + (UInt64)((Float64)(gNumberTimeStamps + 1) * ticksPerPeriod);
    if (next <= now) gNumberTimeStamps++;
    *outSampleTime = (Float64)(gNumberTimeStamps * kRingFrames);
    *outHostTime = gAnchorHostTime + (UInt64)((Float64)gNumberTimeStamps * ticksPerPeriod);
    *outSeed = 1;
    return kAudioHardwareNoError;
}

static OSStatus APUSB_WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 client, UInt32 op,
                                        Boolean *outWillDo, Boolean *outInPlace)
{
    (void)client;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    bool will = (op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix);
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus APUSB_BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 c, UInt32 op, UInt32 n,
                                       const AudioServerPlugInIOCycleInfo *ci)
{
    (void)c; (void)op; (void)n; (void)ci;
    return (d == gDriverRef && id == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}

static OSStatus APUSB_DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, AudioObjectID stream, UInt32 client,
                                    UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *mainBuf, void *secBuf)
{
    (void)client; (void)secBuf;
    if (d != gDriverRef || id != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjectID_Stream_Output) {
        if (mode_has_output())
            apusb_ring_write(&gOutRing, (int64_t)ci->mOutputTime.mSampleTime, (const float *)mainBuf, frames);
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjectID_Stream_Input) {
        if (mode_has_input())
            apusb_ring_read(&gInRing, (int64_t)ci->mInputTime.mSampleTime, (float *)mainBuf, frames, false);
        else
            memset(mainBuf, 0, (size_t)frames * kChannels * sizeof(float));
    }
    return kAudioHardwareNoError;
}

static OSStatus APUSB_EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID id, UInt32 c, UInt32 op, UInt32 n,
                                     const AudioServerPlugInIOCycleInfo *ci)
{
    (void)c; (void)op; (void)n; (void)ci;
    return (d == gDriverRef && id == kObjectID_Device) ? kAudioHardwareNoError : kAudioHardwareBadObjectError;
}
