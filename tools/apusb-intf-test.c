/* Experiment: can a non-root process open interfaces of a device that a
 * root process has captured? SPDX-License-Identifier: MIT */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include "apusb_usb.h"

int main(void)
{
    for (int n = 0; n <= 6; n++) {
        CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostInterface");
        CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int vid = 0x0763, pid = 0x2003;
        CFNumberRef a = CFNumberCreate(NULL, kCFNumberIntType, &vid), b = CFNumberCreate(NULL, kCFNumberIntType, &pid), c = CFNumberCreate(NULL, kCFNumberIntType, &n);
        CFDictionarySetValue(p, CFSTR("idVendor"), a); CFDictionarySetValue(p, CFSTR("idProduct"), b); CFDictionarySetValue(p, CFSTR("bInterfaceNumber"), c);
        CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), p);
        CFRelease(a); CFRelease(b); CFRelease(c); CFRelease(p);
        io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, m);
        if (!s) { printf("if %d: no service\n", n); continue; }
        IOCFPlugInInterface **plug = NULL; SInt32 score;
        IOReturn kr = IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        if (kr) { printf("if %d: create plugin %s\n", n, apusb_strerror(kr)); IOObjectRelease(s); continue; }
        IOUSBInterfaceInterface300 **i = NULL;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&i);
        IODestroyPlugInInterface(plug);
        kr = (*i)->USBInterfaceOpen(i);
        printf("if %d: USBInterfaceOpen %s\n", n, apusb_strerror(kr));
        if (!kr) {
            UInt8 alt = 0; (*i)->GetAlternateSetting(i, &alt);
            printf("      current alt %u\n", alt);
            (*i)->USBInterfaceClose(i);
        }
        (*i)->Release(i);
        IOObjectRelease(s);
    }
    return 0;
}
