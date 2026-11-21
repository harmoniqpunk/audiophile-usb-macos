/*
 * audiophileusbd - tiny root LaunchDaemon that detaches macOS's built-in
 * AppleUSBAudio driver from the M-Audio Audiophile USB so that the
 * community Core Audio / CoreMIDI drivers can open its interfaces.
 *
 * How: IOUSBLib USBDeviceReEnumerate(kUSBReEnumerateCaptureDeviceMask). Apple
 * documents this option for root processes (or apps with the
 * com.apple.vm.device-access entitlement): it terminates every kernel driver
 * attached to the device's interfaces and keeps the device away from them
 * while the capture is held. No kext, no SIP change, no security change.
 *
 * The daemon only holds the capture; it never touches audio data. On SIGTERM
 * (launchctl bootout / uninstall) it hands the device back to macOS.
 *
 * SPDX-License-Identifier: MIT
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/IOMessage.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <os/log.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "apusb_modes.h"
#include "apusb_usb.h"
#include "apusb_reset.h"

static os_log_t gLog;
static IOUSBDeviceInterface650 **gDev = NULL;
static io_service_t gService = 0;
static dispatch_queue_t gQueue;

#define LOGI(fmt, ...) do { os_log(gLog, fmt, ##__VA_ARGS__); } while (0)

static void drop(void)
{
    if (gDev) { (*gDev)->Release(gDev); gDev = NULL; }
    if (gService) { IOObjectRelease(gService); gService = 0; }
}

static void release_device(void)
{
    if (gDev) {
        IOReturn kr = (*gDev)->USBDeviceReEnumerate(gDev, kUSBReEnumerateReleaseDeviceMask);
        LOGI("audiophileusbd: released device to macOS: 0x%08x", kr);
        fprintf(stderr, "audiophileusbd: released device to macOS: %s\n", apusb_strerror(kr));
    }
    drop();
}

static int  gLatchedKey = 0;   /* stream key the device latched since enumeration, 0 = fresh */

static void capture(io_service_t s)
{
    if (gDev) return;
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    IOReturn kr = IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (kr || !plug) { fprintf(stderr, "audiophileusbd: device user client: %s\n", apusb_strerror(kr)); return; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&gDev);
    IODestroyPlugInInterface(plug);
    if (!gDev) return;
    kr = (*gDev)->USBDeviceReEnumerate(gDev, kUSBReEnumerateCaptureDeviceMask);
    LOGI("audiophileusbd: capture Audiophile USB from macOS drivers: 0x%08x", kr);
    fprintf(stderr, "audiophileusbd: capture: %s\n", apusb_strerror(kr));
    if (kr) { drop(); return; }
    IOObjectRetain(s);
    gService = s;
    gLatchedKey = 0;
}

static int gDoneToken = -1;
static uint64_t gResetGeneration = 0;

static int gReqToken = -1;
static int gDefaultMethod = APUSB_RESET_RELEASE;
static bool gWaitingRecapture = false;

static bool has_apple_audio(io_registry_entry_t e, int depth)
{
    if (depth > 4) return false;
    io_iterator_t it = 0; bool found = false;
    if (IORegistryEntryGetChildIterator(e, kIOServicePlane, &it) != KERN_SUCCESS) return false;
    io_registry_entry_t c;
    while (!found && (c = IOIteratorNext(it)) != 0) {
        io_name_t cls;
        if (IOObjectGetClass(c, cls) == KERN_SUCCESS && strncmp(cls, "AppleUSBAudio", 13) == 0) found = true;
        else found = has_apple_audio(c, depth + 1);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
    return found;
}

static void reset_done(IOReturn kr, int method)
{
    usleep(800 * 1000); /* interfaces re-published */
    LOGI("audiophileusbd: device reset (method %d): 0x%08x", method, kr);
    fprintf(stderr, "audiophileusbd: reset (method %d): %s\n", method, apusb_strerror(kr));
    gResetGeneration = (gResetGeneration + 1) | ((uint64_t)(kr == kIOReturnSuccess ? 0 : 1) << 63);
    notify_set_state(gDoneToken, gResetGeneration & ~(1ULL << 63));
    notify_post(APUSB_NOTIFY_RESET_DONE);
}

/* Hardware finding: the Audiophile USB keeps the sample format (16/24-bit) of
 * the first stream after it was enumerated by macOS; ReEnumerate(capture) on
 * an already captured device does not clear that. Several reset methods are
 * implemented so they can be compared on the hardware (apusb-diag reset --method N). */
static void reset_device(uint64_t state)
{
    int method = (int)(state & 0xff), key = (int)((state >> 8) & 0xff);
    if (key && method == APUSB_RESET_DEFAULT && gDev && (gLatchedKey == 0 || gLatchedKey == key)) {
        gLatchedKey = key;      /* fresh device, or already in this format: nothing to do */
        LOGI("audiophileusbd: stream key 0x%02x, no reset needed", key);
        gResetGeneration++;
        notify_set_state(gDoneToken, gResetGeneration);
        notify_post(APUSB_NOTIFY_RESET_DONE);
        return;
    }
    if (method == APUSB_RESET_DEFAULT) method = gDefaultMethod;
    IOReturn kr = kIOReturnNoDevice;
    if (!gDev) { reset_done(kr, method); return; }
    switch (method) {
    case APUSB_RESET_RECAPTURE:
        kr = (*gDev)->USBDeviceReEnumerate(gDev, kUSBReEnumerateCaptureDeviceMask);
        break;
    case APUSB_RESET_SETCONFIG:
    case APUSB_RESET_PORTRESET:
        kr = (*gDev)->USBDeviceOpenSeize(gDev);
        if (kr == kIOReturnSuccess) {
            if (method == APUSB_RESET_SETCONFIG) {
                kr = (*gDev)->SetConfiguration(gDev, 0);
                usleep(200 * 1000);
                IOReturn k2 = (*gDev)->SetConfiguration(gDev, 1);
                if (!kr) kr = k2;
            } else {
                kr = (*gDev)->ResetDevice(gDev);
                usleep(200 * 1000);
                IOReturn k2 = (*gDev)->SetConfiguration(gDev, 1);
                LOGI("audiophileusbd: set configuration after port reset: 0x%08x", k2);
            }
            (*gDev)->USBDeviceClose(gDev);
        }
        break;
    case APUSB_RESET_RELEASE:
    default:
        /* the device is re-enumerated by macOS; on_added() captures the new one */
        gWaitingRecapture = true;
        uint64_t oldId = 0;
        if (gService) IORegistryEntryGetRegistryEntryID(gService, &oldId);
        release_device();
        for (int i = 0; i < 80 && gDev == NULL; i++) {
            /* on_added runs on this serial queue; poll the registry for the new device */
            usleep(100 * 1000);
            io_service_t s = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
            uint64_t id = 0;
            if (s) IORegistryEntryGetRegistryEntryID(s, &id);
            if (s && id != oldId) {
                /* let macOS finish attaching AppleUSBAudio before taking the device
                 * away again (capturing mid-attach once stalled for minutes) */
                for (int k = 0; k < 30 && !has_apple_audio(s, 0); k++) usleep(100 * 1000);
                usleep(700 * 1000);
                capture(s);
            }
            if (s) IOObjectRelease(s);
        }
        gWaitingRecapture = false;
        kr = gDev ? kIOReturnSuccess : kIOReturnNoDevice;
        break;
    }
    if (method == APUSB_RESET_RELEASE) gLatchedKey = key;   /* fresh enumeration, then this stream */
    reset_done(kr, method);
}

/* After system sleep the device's state is unknown (measured: 24-bit capture was
 * garbage after a wake although 24-bit was latched before). Force a real
 * re-enumeration before the next stream. */
static io_connect_t gRootPort = 0;
static void on_power(void *refcon, io_service_t service, natural_t type, void *arg)
{
    (void)refcon; (void)service;
    switch (type) {
    case kIOMessageCanSystemSleep:
    case kIOMessageSystemWillSleep:
        IOAllowPowerChange(gRootPort, (long)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        gLatchedKey = 0xff;   /* unknown: never matches a real key */
        LOGI("audiophileusbd: system woke up; device will be re-enumerated before the next stream");
        break;
    default: break;
    }
}

static void on_added(void *refcon, io_iterator_t it)
{
    (void)refcon;
    io_service_t s;
    while ((s = IOIteratorNext(it)) != 0) {
        /* let the device settle after plug-in before taking it over */
        io_service_t keep = s;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), gQueue, ^{ capture(keep); IOObjectRelease(keep); });
    }
}

static void on_removed(void *refcon, io_iterator_t it)
{
    (void)refcon;
    io_service_t s;
    while ((s = IOIteratorNext(it)) != 0) {
        if (gService && IOObjectIsEqualTo(s, gService)) {
            LOGI("audiophileusbd: device unplugged");
            drop();
        }
        IOObjectRelease(s);
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    gLog = os_log_create("com.harmoniqpunk.audiophileusb", "daemon");
    if (geteuid() != 0) { fprintf(stderr, "audiophileusbd must run as root (it is a LaunchDaemon)\n"); return 1; }
    gQueue = dispatch_queue_create("com.harmoniqpunk.audiophileusbd", DISPATCH_QUEUE_SERIAL);

    IONotificationPortRef port = IONotificationPortCreate(kIOMasterPortDefault);
    IONotificationPortSetDispatchQueue(port, gQueue);
    io_iterator_t added = 0, removed = 0;
    IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, apusb_matching(APUSB_VENDOR_ID, APUSB_PRODUCT_ID), on_added, NULL, &added);
    IOServiceAddMatchingNotification(port, kIOTerminatedNotification, apusb_matching(APUSB_VENDOR_ID, APUSB_PRODUCT_ID), on_removed, NULL, &removed);
    dispatch_sync(gQueue, ^{ on_added(NULL, added); io_service_t s; while ((s = IOIteratorNext(removed))) IOObjectRelease(s); });

    IONotificationPortRef pwrPort = NULL; io_object_t pwrNotifier = 0;
    gRootPort = IORegisterForSystemPower(NULL, &pwrPort, on_power, &pwrNotifier);
    if (gRootPort && pwrPort) IONotificationPortSetDispatchQueue(pwrPort, gQueue);

    notify_register_check(APUSB_NOTIFY_RESET_DONE, &gDoneToken);
    int reqToken = -1;
    notify_register_dispatch(APUSB_NOTIFY_RESET_REQUEST, &reqToken, gQueue, ^(int t) {
        uint64_t m = 0; notify_get_state(t, &m); notify_set_state(t, 0);
        reset_device(m);
    });
    gReqToken = reqToken;

    int sigs[] = { SIGTERM, SIGINT, SIGHUP };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        signal(sigs[i], SIG_IGN);
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0, gQueue);
        dispatch_source_set_event_handler(src, ^{ release_device(); exit(0); });
        dispatch_resume(src);
    }
    LOGI("audiophileusbd: running");
    dispatch_main();
}
