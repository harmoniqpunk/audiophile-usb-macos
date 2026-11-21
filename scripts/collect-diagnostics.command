#!/bin/bash
# Audiophile USB - collect diagnostics (read-only).
# Double-click in Finder (first time: right-click > Open) or run in Terminal.
# Writes a text report to your Desktop (falls back to /Users/Shared) and opens it.
# Nothing is installed, changed, or sent anywhere. USB serial numbers are redacted.
#   --no-open        do not open the report when finished
#   --output DIR     write the report into DIR
# SPDX-License-Identifier: MIT

set -u
OPEN_REPORT=1
OUT_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-open) OPEN_REPORT=0 ;;
        --output) OUT_DIR="${2:-}"; shift ;;
        *) echo "unknown option: $1" >&2 ;;
    esac
    shift
done

PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
STAMP="$(date +%Y%m%d-%H%M%S)"
if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="$HOME/Desktop"
fi
REPORT="$OUT_DIR/AudiophileUSB-diagnostics-$STAMP.txt"
if ! { mkdir -p "$OUT_DIR" && : > "$REPORT"; } 2>/dev/null; then
    OUT_DIR="/Users/Shared/AudiophileUSB"
    mkdir -p "$OUT_DIR"
    REPORT="$OUT_DIR/AudiophileUSB-diagnostics-$STAMP.txt"
    : > "$REPORT"
fi

redact() {
    sed -E \
        -e 's/("?(USB )?Serial Number"?[[:space:]]*[:=][[:space:]]*).*/\1<redacted>/I' \
        -e 's/("?kUSBSerialNumberString"?[[:space:]]*=[[:space:]]*).*/\1<redacted>/' \
        -e 's/("?IOPlatformSerialNumber"?[[:space:]]*=[[:space:]]*).*/\1<redacted>/' \
        -e 's/(Serial Number \(system\):[[:space:]]*).*/\1<redacted>/'
}

section() {
    local title="$1"; shift
    {
        echo
        echo "=================================================================="
        echo "== $title"
        echo "== \$ $*"
        echo "=================================================================="
    } >> "$REPORT"
    "$@" 2>&1 | redact >> "$REPORT"
}

echo "Collecting Audiophile USB diagnostics (read-only)..."
{
    echo "Audiophile USB community driver - diagnostics report"
    echo "Generated: $(date)"
    echo "Review this file before sharing it. Serial numbers are redacted."
} >> "$REPORT"

section "macOS version" sw_vers
section "Kernel / architecture" uname -a
section "Security (SIP) status - read only" csrutil status
section "M-Audio USB devices (apusb-diag)" bash -c 'command -v apusb-diag >/dev/null && apusb-diag list || echo "apusb-diag not installed"'
section "Descriptors (apusb-diag)" bash -c 'command -v apusb-diag >/dev/null && apusb-diag descriptors || echo "apusb-diag not installed"'
section "Interface ownership (apusb-diag)" bash -c 'command -v apusb-diag >/dev/null && apusb-diag claims || echo "apusb-diag not installed"'
section "USB devices" system_profiler SPUSBDataType
section "Audio devices" system_profiler SPAudioDataType
section "IORegistry USB plane" ioreg -p IOUSB -l -w0
section "IORegistry: USB devices with interfaces and attached drivers" ioreg -r -c IOUSBHostDevice -l -w0
section "IORegistry: USB interfaces" ioreg -r -c IOUSBHostInterface -l -w0
section "Loaded kernel extensions (audio / M-Audio)" bash -c 'kextstat 2>/dev/null | grep -i -E "audio|m-audio|maudio|midiman" || echo "(none matched)"'
section "Installed HAL plug-ins" ls -la /Library/Audio/Plug-Ins/HAL
section "Installed MIDI drivers" ls -la "/Library/Audio/MIDI Drivers"
section "Third-party kexts mentioning M-Audio" bash -c 'ls /Library/Extensions 2>/dev/null | grep -i -E "m-audio|maudio|audiophile|midiman" || echo "(none)"'
section "Installed packages (M-Audio / Audiophile)" bash -c 'pkgutil --pkgs | grep -i -E "m-audio|maudio|audiophile|midiman" || echo "(none)"'
section "HAL driver signature" bash -c 'codesign -dv --verbose=2 /Library/Audio/Plug-Ins/HAL/AudiophileUSB.driver 2>&1 || true'
section "HAL driver mode" bash -c 'command -v apusb-diag >/dev/null && apusb-diag get-mode || echo "n/a"'
section "Recent driver log (15 min)" log show --style compact --last 15m --predicate 'subsystem BEGINSWITH "com.harmoniqpunk.audiophileusb" OR eventMessage CONTAINS[c] "AudiophileUSB"'

echo
echo "Report saved to: $REPORT"
if [[ "$OPEN_REPORT" == "1" ]] && command -v open >/dev/null; then
    open -e "$REPORT" 2>/dev/null || open "$REPORT" 2>/dev/null || true
fi
