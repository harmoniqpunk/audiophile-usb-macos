#!/bin/bash
# Build everything and produce dist/AudiophileUSB-<version>.pkg (unsigned, ad-hoc signed binaries).
#   ./build.sh                 build + test + package (x86_64)
#   ARCHS="x86_64 arm64" ./build.sh   universal build
#   ./build.sh --pkg-only      package an existing build/
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")"

VERSION="${VERSION:-0.1.0}"
ARCHS="${ARCHS:-x86_64}"
PKG_ID="com.harmoniqpunk.audiophileusb"
STAGE="build/pkgroot"
DIST="dist"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "build.sh must run on macOS (Xcode or Command Line Tools required)." >&2
    exit 1
fi

if [[ "${1:-}" != "--pkg-only" ]]; then
    make clean
    make test
    make all VERSION="$VERSION" ARCHS="$ARCHS"
    make smoke VERSION="$VERSION" ARCHS="$ARCHS"
fi

rm -rf "$STAGE" "$DIST"
mkdir -p "$STAGE/Library/Audio/Plug-Ins/HAL" \
         "$STAGE/Library/Audio/MIDI Drivers" \
         "$STAGE/Library/Application Support/AudiophileUSB" \
         "$STAGE/usr/local/bin" \
         "$STAGE/Library/LaunchDaemons" \
         "$DIST"

ditto build/AudiophileUSB.driver "$STAGE/Library/Audio/Plug-Ins/HAL/AudiophileUSB.driver"
ditto build/AudiophileUSBMIDI.plugin "$STAGE/Library/Audio/MIDI Drivers/AudiophileUSBMIDI.plugin"
install -m 0755 build/apusb-diag "$STAGE/usr/local/bin/apusb-diag"
install -m 0755 build/audiophileusbd "$STAGE/Library/Application Support/AudiophileUSB/audiophileusbd"
install -m 0644 installer/launchd/com.harmoniqpunk.audiophileusbd.plist "$STAGE/Library/LaunchDaemons/com.harmoniqpunk.audiophileusbd.plist"
install -m 0755 scripts/collect-diagnostics.command "$STAGE/Library/Application Support/AudiophileUSB/collect-diagnostics.command"
install -m 0755 scripts/uninstall.command "$STAGE/Library/Application Support/AudiophileUSB/uninstall.command"
install -m 0644 README.md LICENSE "$STAGE/Library/Application Support/AudiophileUSB/"

# Do not let pkgbuild turn bundles into relocatable components.
COMPONENT_PLIST="build/component.plist"
pkgbuild --analyze --root "$STAGE" "$COMPONENT_PLIST"
i=0
while /usr/libexec/PlistBuddy -c "Print :$i:RootRelativeBundlePath" "$COMPONENT_PLIST" >/dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$COMPONENT_PLIST" 2>/dev/null ||
        /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$COMPONENT_PLIST"
    i=$((i + 1))
done

pkgbuild --root "$STAGE" \
         --component-plist "$COMPONENT_PLIST" \
         --scripts installer/scripts \
         --identifier "$PKG_ID" \
         --version "$VERSION" \
         --install-location / \
         build/AudiophileUSB-component.pkg

sed "s/@VERSION@/$VERSION/g" installer/distribution.xml > build/distribution.xml
rm -rf build/resources && mkdir -p build/resources
cp installer/resources/* build/resources/
cp LICENSE build/resources/LICENSE.txt
productbuild --distribution build/distribution.xml \
             --resources build/resources \
             --package-path build \
             "$DIST/AudiophileUSB-$VERSION.pkg"

# Stand-alone helpers next to the package (no install needed to run diagnostics).
install -m 0755 scripts/collect-diagnostics.command "$DIST/collect-diagnostics.command"
install -m 0755 scripts/uninstall.command "$DIST/uninstall.command"
(cd "$DIST" && shasum -a 256 ./* > SHA256SUMS.txt)

echo
echo "Built $DIST/AudiophileUSB-$VERSION.pkg (UNSIGNED installer, ad-hoc signed binaries, EXPERIMENTAL)"
ls -l "$DIST"
