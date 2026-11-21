/*
 * apusb_reset.h - tiny IPC between the Core Audio driver and audiophileusbd
 * (Darwin notifications, no sockets): the driver asks the daemon to
 * re-enumerate the device, which returns its firmware to a clean state.
 *
 * Hardware finding (2026-11-21): after the device has streamed in one
 * configuration (bit depth / rate), switching to another without a USB
 * re-enumeration produces garbage samples (silent input reads -5 dBFS RMS
 * instead of < -70). A re-enumeration restores clean audio.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef APUSB_RESET_H
#define APUSB_RESET_H

#define APUSB_NOTIFY_RESET_REQUEST "com.harmoniqpunk.audiophileusb.reset-request"
#define APUSB_NOTIFY_RESET_DONE    "com.harmoniqpunk.audiophileusb.reset-done"

/* Reset method, passed as the notify state of APUSB_NOTIFY_RESET_REQUEST
 * (0 = the daemon's default). */
#define APUSB_RESET_DEFAULT      0
#define APUSB_RESET_RECAPTURE    1   /* ReEnumerate(capture) on the captured device */
#define APUSB_RESET_SETCONFIG    2   /* SET_CONFIGURATION 0 -> 1 */
#define APUSB_RESET_PORTRESET    3   /* USB port reset (ResetDevice) */
#define APUSB_RESET_RELEASE      4   /* hand back to macOS, then capture the new device */

/* Request state layout: bits 0-7 method, bits 8-15 stream key (bit depth
 * 16/24 | 0x80 for the 96 kHz half-duplex modes). With a key and the default
 * method the daemon only resets when the device has already latched a
 * different key since it was last enumerated. */
#define APUSB_RESET_STATE(method, key) ((uint64_t)(method) | ((uint64_t)(key) << 8))

#endif
