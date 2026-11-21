#!/bin/bash
# Remove the Audiophile USB community driver. Asks for your admin password.
# Double-click in Finder (first time: right-click > Open) or run in Terminal.
# SPDX-License-Identifier: MIT
set -u

if [[ $EUID -ne 0 ]]; then
    echo "Uninstalling the Audiophile USB community driver needs administrator rights."
    exec sudo /bin/bash "$0" "$@"
fi

echo "Removing files..."
rm -rf "/Library/Audio/Plug-Ins/HAL/AudiophileUSB.driver"
rm -rf "/Library/Audio/MIDI Drivers/AudiophileUSBMIDI.plugin"
rm -f  "/usr/local/bin/apusb-diag"
rm -rf "/Library/Application Support/AudiophileUSB"
pkgutil --forget com.harmoniqpunk.audiophileusb >/dev/null 2>&1 || true

echo "Restarting Core Audio (audio stops for a moment)..."
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod 2>/dev/null || true
killall MIDIServer 2>/dev/null || true

echo "Done. The Audiophile USB community driver has been removed."
