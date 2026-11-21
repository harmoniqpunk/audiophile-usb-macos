/* Experiment: take the Audiophile USB away from AppleUSBAudio with
 * USBDeviceReEnumerate(kUSBReEnumerateCaptureDeviceMask) (root only), hold it
 * for N seconds, then release it back to macOS. SPDX-License-Identifier: MIT */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "apusb_usb.h"

static void tree(io_registry_entry_t e, int d)
{
    io_iterator_t it; io_registry_entry_t c;
    if (IORegistryEntryGetChildIterator(e, kIOServicePlane, &it)) return;
    while ((c = IOIteratorNext(it))) {
        io_name_t cls, name; IOObjectGetClass(c, cls); IORegistryEntryGetName(c, name);
        CFNumberRef n = IORegistryEntryCreateCFProperty(c, CFSTR("bInterfaceNumber"), NULL, 0);
        int num = -1; if (n) { CFNumberGetValue(n, kCFNumberIntType, &num); CFRelease(n); }
        printf("%*s%s \"%s\"%s", d * 2, "", cls, name, num >= 0 ? "" : "\n");
        if (num >= 0) printf(" if=%d\n", num);
        if (d < 3) tree(c, d + 1);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
}

static void show(const char *when)
{
    io_service_t s = apusb_find_service(0x0763, 0x2003);
    printf("--- %s: device %s\n", when, s ? "present" : "ABSENT");
    if (s) { tree(s, 1); IOObjectRelease(s); }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    show("before");
    io_service_t s = apusb_find_service(0x0763, 0x2003);
    if (!s) return 1;
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    IOUSBDeviceInterface650 **dev = NULL;
    IOReturn kr = IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (kr) { printf("plugin: %s\n", apusb_strerror(kr)); return 1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    IODestroyPlugInInterface(plug);
    if (!dev) { printf("no 650 interface\n"); return 1; }
    kr = (*dev)->USBDeviceReEnumerate(dev, kUSBReEnumerateCaptureDeviceMask);
    printf("USBDeviceReEnumerate(capture) -> 0x%08x %s\n", kr, apusb_strerror(kr));
    sleep(2);
    show("after capture");
    printf("holding for %d s (test other clients now)...\n", secs); fflush(stdout);
    sleep(secs);
    show("before release");
    kr = (*dev)->USBDeviceReEnumerate(dev, kUSBReEnumerateReleaseDeviceMask);
    printf("USBDeviceReEnumerate(release) -> 0x%08x %s\n", kr, apusb_strerror(kr));
    (*dev)->Release(dev);
    sleep(3);
    show("after release");
    return 0;
}
