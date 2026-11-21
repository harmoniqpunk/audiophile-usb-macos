# Audiophile USB community driver for macOS

An **unofficial, open-source (MIT)** driver for the **M-Audio Audiophile USB**
(USB ID `0763:2003`) on **macOS 12 Monterey and later** (Intel; Apple Silicon build optional).

> ⚠️ **EXPERIMENTAL · UNSIGNED** — installs no kernel extension and changes no SIP
> or other security setting. Not affiliated with M-Audio / inMusic.

## Status on real hardware (tested 2026-11-21, macOS 12.7.6, MacBook Pro 2015)

> **Experimental and unsigned.** Not yet confirmed by ear. The measurements below
> were made with an RCA loopback cable (analog out -> analog in) and the driver's
> own diagnostics; please report what you actually hear.

**Why the card was noise (all found by measurement):**
1. **AppleUSBAudio reads 24-bit samples in the wrong byte order.** 24-bit is
   big-endian on this card, 16-bit is little-endian (silent input: correct order
   -72 dBFS, wrong order -5 / -24 dBFS).
2. **Our 0.1.0 Core Audio driver read the HAL's buffers as Float32**, but the HAL
   delivers them in the advertised *physical* integer format (16-bit LE / 24-bit
   BE). Decoding the bytes our driver sent over USB gave a 1 kHz test tone an SNR of
   **-7 dB** - the "badly tuned radio". Fixed: the same test now gives
   **66-70 dB** at 24-bit and 59-60 dB at 16-bit (quantisation-limited).
3. **The card latches the bit depth of the first stream after enumeration.**
   Switching 16 <-> 24-bit afterwards gives garbage; rate changes within one depth are
   fine. Only a real re-enumeration (hand the device back to macOS, then capture it
   again) clears it. `audiophileusbd` now does that, only when the bit depth changes
   (about 2-3 s), and after the Mac wakes from sleep.
4. **Output scheduling:** the USB side now reads 2 ms behind "now", so it never
   reads samples Core Audio has not written yet (0 underruns in all tests).

**Loopback results** (1 kHz tone on the left output only, -20/-30 dBFS; numbers from
the driver log, measured on the raw USB data):

| Config | Wire SNR (sent) | Loopback tone | Right input (left-only tone) | Round trip |
|---|---|---|---|---|
| 24-bit 48 kHz | 67 dB | 1000.00 Hz, SNR 30 dB* | -97.8 dBFS (clean) | 658 frames = 13.7 ms (buffer 128) |
| 24-bit 44.1 kHz | 66 dB | 1000.00 Hz | -96.8 dBFS | 610 frames = 13.8 ms (buffer 128) |
| 16-bit 48 kHz | 60 dB | 1000.05 Hz | -93.1 dBFS | 659 frames = 13.7 ms (buffer 128) |
| 16-bit 44.1 kHz | 59 dB | clean when not right after a reset | | 609 frames = 13.8 ms (buffer 128) |

\* The loop loses about 26 dB (analog levels), so the received tone is about -59
dBFS RMS and its SNR is limited by the input noise floor, not by the driver.

- 0 USB errors, 0 resyncs, 0 overloads in normal runs; IO buffers 32-512 frames.
- Tone on the right output appears only on the right input (channel order correct).
- Right after a re-enumeration the analog inputs show a decaying DC offset for a few
  seconds (analog settling).
- Recording through Core Audio could not be checked remotely (macOS microphone
  permission blocks it over SSH); the raw USB input data is verified.
- **MIDI.** Sending a note on/off over USB worked. CoreMIDI-level testing needs a
  logged-in GUI session.
- **Known issues:** a bit-depth change costs 2-3 s and briefly lets macOS's own driver
  see the card. If the Mac sleeps during playback, stop and start playback again.

M-Audio's last Mac driver (1.8.1) supports only OS X 10.6/10.7. This project replaces
it with user-space components:

| Component | Installed to | What it does |
|---|---|---|
| `AudiophileUSB.driver` | `/Library/Audio/Plug-Ins/HAL/` | Core Audio (AudioServerPlugIn) driver: one "Audiophile USB" device, 2 in / 2 out, over IOUSBLib |
| `audiophileusbd` | `/Library/Application Support/AudiophileUSB/` + `/Library/LaunchDaemons/` | Root helper that detaches AppleUSBAudio and resets the device on configuration changes |
| `AudiophileUSBMIDI.plugin` | `/Library/Audio/MIDI Drivers/` | CoreMIDI driver for the MIDI in/out port |
| `apusb-diag` | `/usr/local/bin/` | Diagnostics and direct hardware tests (descriptors, tone, record, MIDI) |
| `collect-diagnostics.command` | `/Library/Application Support/AudiophileUSB/` | Read-only report generator (double-click) |
| `uninstall.command` | `/Library/Application Support/AudiophileUSB/` | Removes everything |

## Install

1. Download `AudiophileUSB-<version>.pkg` from the [Releases](../../releases) page.
2. The package is not notarized, so the first time you open it, **right-click → Open**
   (or System Preferences → Security & Privacy → "Open Anyway").
3. Follow the installer. At the end it:
   * restarts Core Audio (sound stops for about a second), and
   * saves a read-only diagnostics report **`AudiophileUSB-diagnostics-….txt` to your Desktop**
     (or `/Users/Shared/AudiophileUSB` if the Desktop isn't writable).
4. Plug in and power on the Audiophile USB. In System Preferences → Sound (or Audio MIDI Setup),
   pick **"Audiophile USB (experimental)"**.

### Diagnostics without installing anything

Download `collect-diagnostics.command` from the release and double-click it (first time:
right-click → Open). It runs only read-only commands (`sw_vers`, `system_profiler`, `ioreg`,
`kextstat`, `csrutil status`, …), redacts serial numbers, saves the report to your Desktop and opens it.

## Uninstall

Double-click `/Library/Application Support/AudiophileUSB/uninstall.command`
(or the copy in the release). It asks for your password, removes all files listed above,
and restarts Core Audio.

## Modes

The hardware can't run every port at every resolution at once (USB 1.1 bandwidth). Choose a mode:

| Mode | Ports | Format | Rates |
|---|---|---|---|
| `24bit-48k` *(default)* | analog in + analog out | 24-bit | 44.1 / 48 kHz |
| `24bit-48k-spdif-out` | analog in + S/PDIF out | 24-bit | 44.1 / 48 kHz |
| `24bit-48k-spdif-in` | S/PDIF in (needs a live source) + analog out | 24-bit | 44.1 / 48 kHz |
| `16bit-48k` | analog in + analog out | 16-bit | 44.1 / 48 kHz |
| `16bit-48k-spdif-out` | analog in + S/PDIF out | 16-bit | 44.1 / 48 kHz |
| `16bit-48k-spdif-in` | S/PDIF in + analog out | 16-bit | 44.1 / 48 kHz |
| `24bit-96k-play` | analog out only | 24-bit | 88.2 / 96 kHz |
| `24bit-96k-record` | analog in only | 24-bit | 88.2 / 96 kHz |

```sh
apusb-diag modes                 # list
apusb-diag get-mode
apusb-diag set-mode 16bit-48k    # quit audio apps first; power-cycle the box if audio is distorted
```

## Hardware tests (help wanted!)

Quit all audio apps first. These commands talk to the hardware directly:

```sh
apusb-diag list           # is the device seen? (0763:2803 means it's in firmware-update mode)
apusb-diag descriptors    # full USB descriptor dump: please attach to issues
apusb-diag claims         # which macOS drivers own which interface
apusb-diag tone   --mode 24bit-48k --seconds 3                  # 1 kHz tone on analog out
apusb-diag tone   --mode 24bit-48k --format s24le               # if you heard noise: try other byte order
apusb-diag record --mode 24bit-48k --seconds 5 --out test.wav   # record analog in
apusb-diag midi-monitor --seconds 20
apusb-diag midi-test      # sends middle C note on/off
```

## Build from source

Needs Xcode or the Xcode Command Line Tools on macOS 12+.

```sh
make test                      # portable unit tests
make                           # build/AudiophileUSB.driver, build/AudiophileUSBMIDI.plugin, build/apusb-diag (ad-hoc signed)
./build.sh                     # clean + test + build + dist/AudiophileUSB-<version>.pkg
ARCHS="x86_64 arm64" ./build.sh   # universal
```

## Troubleshooting

* **Device doesn't appear in Sound settings.** Run the diagnostics. `apusb-diag list` must show
  `0763:2003`. If it shows `0763:2803`, the device is in firmware-update mode and needs M-Audio's
  firmware loader, which this project doesn't include.
* **`exclusive access` errors / `apusb-diag claims` shows `AppleUSBAudio…` under interfaces 2–5.**
  The built-in macOS USB-audio driver grabbed the audio interfaces. Because they look standard,
  you may see a second "USB Audio" device that plays distorted sound (wrong byte order).
  This driver can't share them. Try `apusb-diag tone --seize`. A permanent fix would need a
  small matching-blocker kext, which requires Apple-granted signing or reduced security.
  That isn't done here; please report it in an issue.
* **Noise instead of sound.** The byte order may differ from the documentation. Try
  `apusb-diag tone --format s24le` (or `s16be`/`s16le` in 16-bit modes) and report which one works.
* **Distorted input / wrong pitch after changing modes.** Quit audio apps, power-cycle the
  interface, and use the S/PDIF-in modes only with a live digital source connected.
* **Clicks every few minutes.** Expected in this version: the device clock is derived from the
  host clock and corrected by ±1 sample.
* **Logs:** `log stream --predicate 'subsystem BEGINSWITH "com.harmoniqpunk.audiophileusb"'`
  (Core Audio messages are tagged `AudiophileUSB:`).

## How it works

* `src/common/apusb_usb.c`: finds the device, selects configuration 1 if it isn't configured,
  opens interfaces, and sends the USB Audio 1.0 `SET_CUR` sampling-frequency request.
* `src/common/apusb_stream.c`: isochronous engine, 6 transfers × 8 ms in flight on its own run-loop thread.
* `src/hal/AudiophileUSBDriver.c`: AudioServerPlugIn; float ring buffers ↔ device PCM
  (24-bit big-endian / 16-bit little-endian); the playback rate follows the host clock via ±1-sample packet sizing.
* `src/midi/AudiophileUSBMIDI.c`: CoreMIDI driver for the vendor "Midiman" 4-byte framing on interface 6.
* Device facts and sources: [docs/PROTOCOL.md](docs/PROTOCOL.md). Clean-room: written from
  documented behaviour only. No code from the official driver or GPL projects.

## License

MIT, © 2026 Harmoniq Punk. See [LICENSE](LICENSE). "M-Audio" and "Audiophile" are trademarks of
their respective owners and are used only to identify compatible hardware.
