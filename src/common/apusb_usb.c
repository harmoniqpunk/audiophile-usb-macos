/*
 * apusb_usb.c - IOUSBLib helpers.
 * SPDX-License-Identifier: MIT
 */
#include "apusb_usb.h"

#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

CFMutableDictionaryRef apusb_matching(UInt16 vendorID, UInt16 productID)
{
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    if (m == NULL) return NULL;
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SInt32 v = vendorID, p = productID;
    CFNumberRef nv = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(props, CFSTR(kUSBVendorID), nv);
    CFRelease(nv);
    if (productID != 0) {
        CFNumberRef np = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &p);
        CFDictionarySetValue(props, CFSTR(kUSBProductID), np);
        CFRelease(np);
    }
    CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props);
    return m;
}

io_service_t apusb_find_service(UInt16 vendorID, UInt16 productID)
{
    CFMutableDictionaryRef m = apusb_matching(vendorID, productID);
    if (m == NULL) return 0;
    /* IOServiceGetMatchingService consumes the reference to m. */
    return IOServiceGetMatchingService(kIOMasterPortDefault, m);
}

IOReturn apusb_device_create(io_service_t service, apusb_device *out)
{
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    memset(out, 0, sizeof *out);
    IOReturn kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                                    kIOCFPlugInInterfaceID, &plug, &score);
    if (kr != kIOReturnSuccess || plug == NULL) return kr ? kr : kIOReturnError;
    HRESULT hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                         (LPVOID *)&out->dev);
    IODestroyPlugInInterface(plug);
    if (hr != S_OK || out->dev == NULL) return kIOReturnUnsupported;
    IOObjectRetain(service);
    out->service = service;
    (*out->dev)->GetDeviceVendor(out->dev, &out->vendorID);
    (*out->dev)->GetDeviceProduct(out->dev, &out->productID);
    (*out->dev)->GetLocationID(out->dev, &out->locationID);
    return kIOReturnSuccess;
}

void apusb_device_release(apusb_device *d)
{
    if (d->dev) (*d->dev)->Release(d->dev);
    if (d->service) IOObjectRelease(d->service);
    memset(d, 0, sizeof *d);
}

IOReturn apusb_device_ensure_configured(apusb_device *d)
{
    UInt8 current = 0;
    IOReturn kr = (*d->dev)->GetConfiguration(d->dev, &current);
    if (kr == kIOReturnSuccess && current != 0) return kIOReturnSuccess;

    IOUSBConfigurationDescriptorPtr cfg = NULL;
    kr = (*d->dev)->GetConfigurationDescriptorPtr(d->dev, 0, &cfg);
    if (kr != kIOReturnSuccess || cfg == NULL) return kr ? kr : kIOReturnError;

    kr = (*d->dev)->USBDeviceOpen(d->dev);
    if (kr != kIOReturnSuccess) return kr; /* someone else owns the device */
    kr = (*d->dev)->SetConfiguration(d->dev, cfg->bConfigurationValue);
    (*d->dev)->USBDeviceClose(d->dev);
    if (kr == kIOReturnSuccess) usleep(200 * 1000); /* let IOKit publish interfaces */
    return kr;
}

static IOReturn find_interface(apusb_device *d, UInt8 number, apusb_intf_ref *out)
{
    IOUSBFindInterfaceRequest req = {
        .bInterfaceClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
        .bAlternateSetting = kIOUSBFindInterfaceDontCare,
    };
    io_iterator_t it = 0;
    IOReturn kr = (*d->dev)->CreateInterfaceIterator(d->dev, &req, &it);
    if (kr != kIOReturnSuccess) return kr;

    io_service_t s;
    IOReturn result = kIOReturnNotFound;
    while ((s = IOIteratorNext(it)) != 0) {
        IOCFPlugInInterface **plug = NULL;
        SInt32 score = 0;
        apusb_intf_ref intf = NULL;
        if (IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID,
                                              kIOCFPlugInInterfaceID, &plug, &score) == kIOReturnSuccess && plug) {
            (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300),
                                    (LPVOID *)&intf);
            IODestroyPlugInInterface(plug);
        }
        IOObjectRelease(s);
        if (intf == NULL) continue;
        UInt8 n = 0xff;
        (*intf)->GetInterfaceNumber(intf, &n);
        if (n == number) { *out = intf; result = kIOReturnSuccess; break; }
        (*intf)->Release(intf);
    }
    IOObjectRelease(it);
    return result;
}

IOReturn apusb_interface_open(apusb_device *d, UInt8 number, bool seize, apusb_interface *out)
{
    memset(out, 0, sizeof *out);
    IOReturn kr = kIOReturnNotFound;
    for (int attempt = 0; attempt < 20; attempt++) {
        kr = find_interface(d, number, &out->intf);
        if (kr == kIOReturnSuccess) break;
        usleep(100 * 1000);
    }
    if (kr != kIOReturnSuccess) return kr;
    out->number = number;
    kr = seize ? (*out->intf)->USBInterfaceOpenSeize(out->intf)
               : (*out->intf)->USBInterfaceOpen(out->intf);
    if (kr != kIOReturnSuccess) {
        (*out->intf)->Release(out->intf);
        out->intf = NULL;
        return kr;
    }
    out->isOpen = true;
    return kIOReturnSuccess;
}

void apusb_interface_close(apusb_interface *i)
{
    if (i->intf) {
        if (i->isOpen) (*i->intf)->USBInterfaceClose(i->intf);
        (*i->intf)->Release(i->intf);
    }
    memset(i, 0, sizeof *i);
}

IOReturn apusb_interface_set_alt(apusb_interface *i, UInt8 alt)
{
    return (*i->intf)->SetAlternateInterface(i->intf, alt);
}

IOReturn apusb_interface_find_pipe(apusb_interface *i, UInt8 direction, UInt8 transferType, apusb_pipe *out)
{
    UInt8 count = 0;
    IOReturn kr = (*i->intf)->GetNumEndpoints(i->intf, &count);
    if (kr != kIOReturnSuccess) return kr;
    for (UInt8 p = 1; p <= count; p++) {
        UInt8 dir = 0, num = 0, type = 0, interval = 0;
        UInt16 mps = 0;
        if ((*i->intf)->GetPipeProperties(i->intf, p, &dir, &num, &type, &mps, &interval) != kIOReturnSuccess)
            continue;
        if (dir == direction && type == transferType) {
            out->pipeRef = p;
            out->address = (UInt8)(num | (dir == kUSBIn ? 0x80 : 0x00));
            out->transferType = type;
            out->maxPacketSize = mps;
            return kIOReturnSuccess;
        }
    }
    return kIOReturnNotFound;
}

IOReturn apusb_set_sample_rate(apusb_interface *i, UInt8 endpointAddress, UInt32 rate)
{
    UInt8 data[3] = { (UInt8)(rate & 0xff), (UInt8)((rate >> 8) & 0xff), (UInt8)((rate >> 16) & 0xff) };
    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint), /* 0x22 */
        .bRequest = 0x01,   /* SET_CUR */
        .wValue = 0x0100,   /* SAMPLING_FREQ_CONTROL << 8 */
        .wIndex = endpointAddress,
        .wLength = 3,
        .pData = data,
    };
    return (*i->intf)->ControlRequest(i->intf, 0, &req);
}

IOReturn apusb_get_sample_rate(apusb_interface *i, UInt8 endpointAddress, UInt32 *rate)
{
    UInt8 data[3] = { 0, 0, 0 };
    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBClass, kUSBEndpoint), /* 0xA2 */
        .bRequest = 0x81,   /* GET_CUR */
        .wValue = 0x0100,
        .wIndex = endpointAddress,
        .wLength = 3,
        .pData = data,
    };
    IOReturn kr = (*i->intf)->ControlRequest(i->intf, 0, &req);
    if (kr == kIOReturnSuccess) *rate = (UInt32)data[0] | ((UInt32)data[1] << 8) | ((UInt32)data[2] << 16);
    return kr;
}

const char *apusb_strerror(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess: return "success";
    case kIOReturnExclusiveAccess: return "exclusive access (another driver owns it)";
    case kIOReturnNotFound: return "not found";
    case kIOReturnNotResponding: return "device not responding";
    case kIOReturnNoDevice: return "no device";
    case kIOReturnNotPrivileged: return "not privileged";
    case kIOReturnNotPermitted: return "not permitted (sandbox?)";
    case kIOReturnUnsupported: return "unsupported";
    case kIOReturnNoBandwidth: return "no USB bandwidth";
    case kIOReturnIsoTooOld: return "isochronous frame too old";
    case kIOReturnIsoTooNew: return "isochronous frame too new";
    case kIOReturnAborted: return "aborted";
    default: {
        static __thread char buf[32];
        snprintf(buf, sizeof buf, "0x%08x", (unsigned)r);
        return buf;
    }
    }
}
