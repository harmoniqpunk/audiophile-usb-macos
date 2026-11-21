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

#endif
