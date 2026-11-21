/*
 * AudiophileUSBMIDI.c - experimental CoreMIDI driver plug-in for the MIDI
 * port of the M-Audio Audiophile USB (USB ID 0763:2003, interface 6).
 *
 * Interface 6 is vendor specific: one interrupt-IN endpoint carries all
 * incoming MIDI and one bulk-OUT endpoint carries outgoing MIDI, both
 * using 4-byte "Midiman" packets (see src/common/apusb_midiman.h).
 *
 * EXPERIMENTAL - UNSIGNED - UNTESTED ON HARDWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIDriver.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "apusb_midiman.h"
#include "apusb_modes.h"
#include "apusb_usb.h"

#define kDriverModel "Audiophile USB MIDI"

static os_log_t gLog;

typedef struct {
    MIDIDriverInterface  *iface;     /* must be first: MIDIDriverRef points here */
    UInt32                refCount;
    MIDIDeviceRef         device;
    MIDIEndpointRef       source;
    MIDIEndpointRef       dest;
    apusb_device          usb;
    apusb_interface       intf;
    apusb_pipe            inPipe, outPipe;
    CFRunLoopSourceRef    asyncSource;
    uint8_t               inBuf[64];
    bool                  online;
    bool                  stopping;
    pthread_mutex_t       sendLock;
    apusb_midiman_encoder enc;
    IONotificationPortRef notify;
    io_iterator_t         addedIter, removedIter;
} APMIDIDriver;

static APMIDIDriver *gSelf;

/* ------------------------------------------------------------------------ */

static void midi_sink(void *ctx, unsigned port, const uint8_t *bytes, unsigned len)
{
    APMIDIDriver *self = (APMIDIDriver *)ctx;
    (void)port; /* single port device */
    if (!self->source) return;
    Byte buf[64];
    MIDIPacketList *pl = (MIDIPacketList *)buf;
    MIDIPacket *p = MIDIPacketListInit(pl);
    p = MIDIPacketListAdd(pl, sizeof buf, p, mach_absolute_time(), len, bytes);
    if (p) MIDIReceived(self->source, pl);
}

static void read_complete(void *refcon, IOReturn result, void *arg0);

static IOReturn submit_read(APMIDIDriver *self)
{
    UInt32 len = self->inPipe.maxPacketSize;
    if (len > sizeof self->inBuf) len = sizeof self->inBuf;
    return (*self->intf.intf)->ReadPipeAsync(self->intf.intf, self->inPipe.pipeRef, self->inBuf, len,
                                             read_complete, self);
}

static void read_complete(void *refcon, IOReturn result, void *arg0)
{
    APMIDIDriver *self = (APMIDIDriver *)refcon;
    UInt32 got = (UInt32)(uintptr_t)arg0;
    if (result == kIOReturnSuccess && got > 0)
        apusb_midiman_decode(self->inBuf, got, midi_sink, self);
    if (self->stopping || result == kIOReturnAborted || result == kIOReturnNoDevice) return;
    if (result == kIOUSBPipeStalled)
        (*self->intf.intf)->ClearPipeStallBothEnds(self->intf.intf, self->inPipe.pipeRef);
    IOReturn kr = submit_read(self);
    if (kr) os_log_error(gLog, "AudiophileUSB MIDI: resubmit read failed: %{public}s", apusb_strerror(kr));
}

static void usb_close(APMIDIDriver *self)
{
    self->stopping = true;
    if (self->intf.intf) {
        (*self->intf.intf)->AbortPipe(self->intf.intf, self->inPipe.pipeRef);
        if (self->asyncSource) {
            CFRunLoopRemoveSource(MIDIGetDriverIORunLoop(), self->asyncSource, kCFRunLoopDefaultMode);
            CFRelease(self->asyncSource);
            self->asyncSource = NULL;
        }
        apusb_interface_close(&self->intf);
    }
    if (self->usb.dev) apusb_device_release(&self->usb);
    self->online = false;
    if (self->device) MIDIObjectSetIntegerProperty(self->device, kMIDIPropertyOffline, 1);
}

static bool usb_open(APMIDIDriver *self)
{
    if (self->online) return true;
    io_service_t svc = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
    if (!svc) return false;
    IOReturn kr = apusb_device_create(svc, &self->usb);
    IOObjectRelease(svc);
    if (kr) return false;
    apusb_device_ensure_configured(&self->usb);
    kr = apusb_interface_open(&self->usb, APUSB_IF_MIDI, false, &self->intf);
    if (kr) {
        os_log_error(gLog, "AudiophileUSB MIDI: open interface 6: %{public}s", apusb_strerror(kr));
        apusb_device_release(&self->usb);
        return false;
    }
    if (apusb_interface_find_pipe(&self->intf, kUSBIn, kUSBInterrupt, &self->inPipe) ||
        apusb_interface_find_pipe(&self->intf, kUSBOut, kUSBBulk, &self->outPipe)) {
        os_log_error(gLog, "AudiophileUSB MIDI: expected interrupt-IN and bulk-OUT endpoints not found");
        usb_close(self);
        return false;
    }
    if ((*self->intf.intf)->CreateInterfaceAsyncEventSource(self->intf.intf, &self->asyncSource) == kIOReturnSuccess)
        CFRunLoopAddSource(MIDIGetDriverIORunLoop(), self->asyncSource, kCFRunLoopDefaultMode);
    self->stopping = false;
    apusb_midiman_encoder_init(&self->enc, 0);
    kr = submit_read(self);
    if (kr) {
        os_log_error(gLog, "AudiophileUSB MIDI: start read: %{public}s", apusb_strerror(kr));
        usb_close(self);
        return false;
    }
    self->online = true;
    if (self->device) MIDIObjectSetIntegerProperty(self->device, kMIDIPropertyOffline, 0);
    os_log(gLog, "AudiophileUSB MIDI: online");
    return true;
}

/* Hot-plug ---------------------------------------------------------------- */

static void drain(io_iterator_t it) { io_service_t s; while ((s = IOIteratorNext(it))) IOObjectRelease(s); }

static void on_added(void *refcon, io_iterator_t it)
{
    drain(it);
    APMIDIDriver *self = (APMIDIDriver *)refcon;
    if (self->device) usb_open(self);
}

static void on_removed(void *refcon, io_iterator_t it)
{
    drain(it);
    usb_close((APMIDIDriver *)refcon);
}

static CFMutableDictionaryRef usb_matching(void)
{
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    SInt32 v = APUSB_VENDOR_ID, p = APUSB_PRODUCT_ID;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    CFRelease(nv);
    CFRelease(np);
    return m;
}

/* MIDIDriverInterface ------------------------------------------------------ */

static HRESULT APM_QueryInterface(void *thisPtr, REFIID iid, LPVOID *ppv);
static ULONG   APM_AddRef(void *thisPtr);
static ULONG   APM_Release(void *thisPtr);

static OSStatus APM_FindDevices(MIDIDriverRef ref, MIDIDeviceListRef devList)
{
    (void)ref;
    io_service_t svc = apusb_find_service(APUSB_VENDOR_ID, APUSB_PRODUCT_ID);
    if (!svc) return noErr;
    IOObjectRelease(svc);
    MIDIDeviceRef dev;
    MIDIEntityRef ent;
    if (MIDIDeviceCreate(ref, CFSTR("Audiophile USB"), CFSTR("M-Audio"), CFSTR(kDriverModel), &dev) != noErr)
        return noErr;
    MIDIDeviceAddEntity(dev, CFSTR("Audiophile USB MIDI"), false, 1, 1, &ent);
    MIDIDeviceListAddDevice(devList, dev);
    return noErr;
}

static OSStatus APM_Start(MIDIDriverRef ref, MIDIDeviceListRef devList)
{
    APMIDIDriver *self = (APMIDIDriver *)ref;
    ItemCount n = MIDIDeviceListGetNumberOfDevices(devList);
    for (ItemCount i = 0; i < n; i++) {
        MIDIDeviceRef dev = MIDIDeviceListGetDevice(devList, i);
        CFStringRef model = NULL;
        if (MIDIObjectGetStringProperty(dev, kMIDIPropertyModel, &model) != noErr || model == NULL) continue;
        bool ours = CFStringCompare(model, CFSTR(kDriverModel), 0) == kCFCompareEqualTo;
        CFRelease(model);
        if (!ours || MIDIDeviceGetNumberOfEntities(dev) < 1) continue;
        MIDIEntityRef ent = MIDIDeviceGetEntity(dev, 0);
        self->device = dev;
        self->source = MIDIEntityGetSource(ent, 0);
        self->dest = MIDIEntityGetDestination(ent, 0);
        if (self->dest) MIDIEndpointSetRefCons(self->dest, self, NULL);
        break;
    }
    if (self->device == 0) return noErr;

    self->notify = IONotificationPortCreate(kIOMasterPortDefault);
    if (self->notify) {
        CFRunLoopAddSource(MIDIGetDriverIORunLoop(), IONotificationPortGetRunLoopSource(self->notify), kCFRunLoopDefaultMode);
        if (IOServiceAddMatchingNotification(self->notify, kIOFirstMatchNotification, usb_matching(), on_added, self, &self->addedIter) == kIOReturnSuccess)
            drain(self->addedIter);
        if (IOServiceAddMatchingNotification(self->notify, kIOTerminatedNotification, usb_matching(), on_removed, self, &self->removedIter) == kIOReturnSuccess)
            drain(self->removedIter);
    }
    if (!usb_open(self)) MIDIObjectSetIntegerProperty(self->device, kMIDIPropertyOffline, 1);
    return noErr;
}

static OSStatus APM_Stop(MIDIDriverRef ref)
{
    APMIDIDriver *self = (APMIDIDriver *)ref;
    usb_close(self);
    if (self->addedIter) { IOObjectRelease(self->addedIter); self->addedIter = 0; }
    if (self->removedIter) { IOObjectRelease(self->removedIter); self->removedIter = 0; }
    if (self->notify) {
        CFRunLoopRemoveSource(MIDIGetDriverIORunLoop(), IONotificationPortGetRunLoopSource(self->notify), kCFRunLoopDefaultMode);
        IONotificationPortDestroy(self->notify);
        self->notify = NULL;
    }
    return noErr;
}

static OSStatus APM_Configure(MIDIDriverRef ref, MIDIDeviceRef dev) { (void)ref; (void)dev; return noErr; }

static OSStatus APM_Send(MIDIDriverRef ref, const MIDIPacketList *pl, void *destRef1, void *destRef2)
{
    (void)destRef2;
    APMIDIDriver *self = destRef1 ? (APMIDIDriver *)destRef1 : (APMIDIDriver *)ref;
    if (!self->online || pl == NULL) return noErr;
    uint8_t out[4 * 260];
    pthread_mutex_lock(&self->sendLock);
    const MIDIPacket *p = &pl->packet[0];
    for (UInt32 i = 0; i < pl->numPackets; i++) {
        size_t off = 0;
        while (off < p->length) {
            size_t chunk = p->length - off;
            if (chunk > 256) chunk = 256;
            size_t n = apusb_midiman_encode(&self->enc, p->data + off, chunk, out, sizeof out);
            off += chunk;
            for (size_t w = 0; w < n; ) {
                UInt32 len = (UInt32)(n - w);
                if (len > self->outPipe.maxPacketSize) len = self->outPipe.maxPacketSize & ~3u;
                if (len == 0) break;
                IOReturn kr = (*self->intf.intf)->WritePipe(self->intf.intf, self->outPipe.pipeRef, out + w, len);
                if (kr) { os_log_error(gLog, "AudiophileUSB MIDI: write: %{public}s", apusb_strerror(kr)); break; }
                w += len;
            }
        }
        p = MIDIPacketNext(p);
    }
    pthread_mutex_unlock(&self->sendLock);
    return noErr;
}

static OSStatus APM_EnableSource(MIDIDriverRef ref, MIDIEndpointRef src, Boolean enabled)
{ (void)ref; (void)src; (void)enabled; return noErr; }
static OSStatus APM_Flush(MIDIDriverRef ref, MIDIEndpointRef dest, void *r1, void *r2)
{ (void)ref; (void)dest; (void)r1; (void)r2; return noErr; }
static OSStatus APM_Monitor(MIDIDriverRef ref, MIDIEndpointRef dest, const MIDIPacketList *pl)
{ (void)ref; (void)dest; (void)pl; return noErr; }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
static MIDIDriverInterface gInterface = {
    NULL,
    APM_QueryInterface, APM_AddRef, APM_Release,
    APM_FindDevices, APM_Start, APM_Stop, APM_Configure, APM_Send,
    APM_EnableSource, APM_Flush, APM_Monitor,
    /* remaining (newer SDK) members are zero-initialised */
};
#pragma clang diagnostic pop

static HRESULT APM_QueryInterface(void *thisPtr, REFIID iid, LPVOID *ppv)
{
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, kMIDIDriverInterfaceID) || CFEqual(req, IUnknownUUID)) {
        APMIDIDriver *self = (APMIDIDriver *)thisPtr;
        self->refCount++;
        *ppv = thisPtr;
        r = S_OK;
    } else {
        *ppv = NULL;
    }
    CFRelease(req);
    return r;
}

static ULONG APM_AddRef(void *thisPtr) { return ++((APMIDIDriver *)thisPtr)->refCount; }

static ULONG APM_Release(void *thisPtr)
{
    APMIDIDriver *self = (APMIDIDriver *)thisPtr;
    if (self->refCount > 0) self->refCount--;
    return self->refCount;
}

__attribute__((visibility("default"))) void *AudiophileUSBMIDI_Create(CFAllocatorRef allocator, CFUUIDRef typeID);
__attribute__((visibility("default"))) void *AudiophileUSBMIDI_Create(CFAllocatorRef allocator, CFUUIDRef typeID)
{
    (void)allocator;
    if (!CFEqual(typeID, kMIDIDriverTypeID)) return NULL;
    if (gSelf == NULL) {
        gLog = os_log_create("com.harmoniqpunk.audiophileusb.midi", "driver");
        gSelf = (APMIDIDriver *)calloc(1, sizeof *gSelf);
        if (gSelf == NULL) return NULL;
        gSelf->iface = &gInterface;
        pthread_mutex_init(&gSelf->sendLock, NULL);
    }
    gSelf->refCount++;
    return gSelf;
}
