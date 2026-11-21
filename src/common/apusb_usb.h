/*
 * apusb_usb.h - thin IOUSBLib (user-space USB) helpers for the
 * Audiophile USB. macOS only.
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_USB_H
#define APUSB_USB_H

#include <stdbool.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef IOUSBDeviceInterface       **apusb_dev_ref;
typedef IOUSBInterfaceInterface300 **apusb_intf_ref;

typedef struct {
    apusb_dev_ref dev;
    io_service_t  service;
    UInt16        vendorID;
    UInt16        productID;
    UInt32        locationID;
} apusb_device;

typedef struct {
    apusb_intf_ref intf;
    UInt8          number;
    bool           isOpen;
} apusb_interface;

typedef struct {
    UInt8  pipeRef;
    UInt8  address;     /* bEndpointAddress, including direction bit */
    UInt8  transferType;
    UInt16 maxPacketSize;
} apusb_pipe;

/* Returns the first service matching vid/pid (caller releases), or 0. */
io_service_t apusb_find_service(UInt16 vendorID, UInt16 productID);

IOReturn apusb_device_create(io_service_t service, apusb_device *out);
void     apusb_device_release(apusb_device *d);
/* Selects configuration 1 if the device is still unconfigured. */
IOReturn apusb_device_ensure_configured(apusb_device *d);

/* Opens interface `number` (retries ~2 s while IOKit publishes nubs). */
IOReturn apusb_interface_open(apusb_device *d, UInt8 number, bool seize, apusb_interface *out);
void     apusb_interface_close(apusb_interface *i);
IOReturn apusb_interface_set_alt(apusb_interface *i, UInt8 alt);
/* Finds the first pipe with given direction (kUSBIn/kUSBOut) and type (kUSBIsoc...). */
IOReturn apusb_interface_find_pipe(apusb_interface *i, UInt8 direction, UInt8 transferType, apusb_pipe *out);

/* USB Audio 1.0 SET_CUR SAMPLING_FREQ_CONTROL to an endpoint. */
IOReturn apusb_set_sample_rate(apusb_interface *i, UInt8 endpointAddress, UInt32 rate);
IOReturn apusb_get_sample_rate(apusb_interface *i, UInt8 endpointAddress, UInt32 *rate);

const char *apusb_strerror(IOReturn r);

#ifdef __cplusplus
}
#endif
#endif
