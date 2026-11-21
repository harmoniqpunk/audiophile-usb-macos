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
}

static int gDoneToken = -1;
static uint64_t gResetGeneration = 0;

/* Re-enumerate the (captured) device: a USB reset returns the firmware to a
 * clean state. Capturing again re-enumerates without ever letting
 * AppleUSBAudio attach in between. */
static void reset_device(void)
{
    IOReturn kr = kIOReturnNoDevice;
    if (gDev) {
        kr = (*gDev)->USBDeviceReEnumerate(gDev, kUSBReEnumerateCaptureDeviceMask);
        if (kr != kIOReturnSuccess) {
            /* fall back: hand back and take again */
            io_service_t s = gService; if (s) IOObjectRetain(s);
            release_device();
            usleep(1500 * 1000);
            io_service_t now = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
            if (now) { capture(now); IOObjectRelease(now); kr = gDev ? kIOReturnSuccess : kIOReturnError; }
            if (s) IOObjectRelease(s);
        }
    }
    usleep(800 * 1000); /* interfaces re-published */
    LOGI("audiophileusbd: device reset (re-enumerate): 0x%08x", kr);
    fprintf(stderr, "audiophileusbd: reset: %s\n", apusb_strerror(kr));
    gResetGeneration = (gResetGeneration + 1) | ((uint64_t)(kr == kIOReturnSuccess ? 0 : 1) << 63);
    notify_set_state(gDoneToken, gResetGeneration & ~(1ULL << 63));
    notify_post(APUSB_NOTIFY_RESET_DONE);
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

    notify_register_check(APUSB_NOTIFY_RESET_DONE, &gDoneToken);
    int reqToken = -1;
    notify_register_dispatch(APUSB_NOTIFY_RESET_REQUEST, &reqToken, gQueue, ^(int t) { (void)t; reset_device(); });

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
