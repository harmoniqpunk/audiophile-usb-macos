# Audiophile USB – device notes (clean-room)

These notes describe the **behaviour** of the M-Audio Audiophile USB as publicly
documented. The driver in this repository was written from these facts only;
no code from other drivers (including the GPL Linux driver) was copied.

## Identity

| Item | Value | Source |
|---|---|---|
| USB vendor / product | `0763:2003` ("M Audio AudioPhile") | [usb.ids](https://usb-ids.gowdy.us/read/UD/0763/2003) |
| Firmware-update (DFU) identity | `0763:2803` ("M-Audio Audiophile DFU") | [usb.ids vendor list](https://hardwareidlookup.online/usb/0763/) |
| Bus | USB 1.1 full speed (1 ms isochronous frames) | [kernel docs][kdoc] |

Not to be confused with the **FireWire Audiophile** (FireWire / BridgeCo) or the
**Delta Audiophile 2496 / 192** PCI cards (ICE1712 / ICE1724). This project only
targets the USB model.

## Capabilities ([kernel docs][kdoc])

* Analog stereo in (Ai), analog stereo out (Ao), S/PDIF in (Di), S/PDIF out (Do), MIDI in/out.
* 16 or 24 bit, 8–96 kHz. All active interfaces must use the same bit depth.
* USB 1.1 bandwidth limits:
  * 16-bit/48 kHz: Ai+Ao+Di+Do
  * 24-bit/48 kHz: any three of the four ports
  * 24-bit/96 kHz: **one** port only (half duplex)
* Di should only be enabled with an active S/PDIF source (it becomes the clock source).
* Users must close audio applications before changing bit depth; mode changes are
  most reliable when the interface is power-cycled.

## USB layout ([kernel docs][kdoc], "USB implementation details")

| Interface | Function |
|---|---|
| 0 | vendor ("USB interface") |
| 1 | USB Audio Class 1.0 AudioControl |
| 2 | analog out (AudioStreaming) |
| 3 | digital out (AudioStreaming; extra alt 6 = AC-3/DTS pass-through) |
| 4 | analog in (AudioStreaming) |
| 5 | digital in (AudioStreaming; **no** alt 3 and 5) |
| 6 | MIDI, vendor-specific "Midiman" framing |

Alternate settings (from the documented `device_setup` mode table):

| Alt | Meaning |
|---|---|
| 1 | 24-bit, 48.1–96 kHz (one port at a time) |
| 2 | 24-bit, ≤48 kHz, Di enabled |
| 3 | 24-bit, ≤48 kHz, Di disabled |
| 4 | 16-bit, ≤48 kHz, Di enabled |
| 5 | 16-bit, ≤48 kHz, Di disabled |
| 6 | 16-bit AC-3/DTS pass-through on Do |

Documented quirks the driver must handle:

1. **Reset all streaming interfaces to alt 0** before selecting the working alternate setting.
2. **24-bit samples are big-endian** (3 bytes, MSB first). 16-bit samples are little-endian
   in explicitly selected modes (big-endian on alt 6 in the default Linux mode).
   `apusb-diag tone/record --format …` exists to verify this on real hardware.
3. Endpoints accept the UAC1 **sampling-frequency control** (`SET_CUR`, `0x22/0x01`,
   `wValue 0x0100`, `wIndex` = endpoint, 3-byte little-endian rate) although their
   descriptors don't advertise it.
4. Playback is treated as **adaptive**, capture as **synchronous** (no feedback endpoint).

## MIDI framing

Facts as observable in the publicly documented behaviour of the Linux
`snd-usb-audio` "Midiman" protocol (no code reused):

* Interface 6, endpoint index 0 = **interrupt IN** (all incoming MIDI),
  endpoint index 2 = **bulk OUT** (port 0). Indexes 1 and 3 are unused bulk IN.
* Each 4-byte packet: bytes 0–2 = raw MIDI bytes, byte 3 = `(port << 4) | count`
  where `count` = number of valid bytes (1–3). Byte 3 = 0 → empty slot.

## Official drivers (for reference only, not redistributed)

* Last macOS driver: **1.8.1** (`Audiophile_USB_OSX_1.8.1.dmg`), OS X 10.6/10.7, listed on
  [rebyte.me (third-party mirror)](https://rebyte.me/en/m-audio/118873/).
* [M-Audio legacy downloads](https://www.m-audio.com/support/legacy-downloads) – the Audiophile USB
  entry exists but its Mac link was an unfilled placeholder when checked (Nov 2026).

[kdoc]: https://docs.kernel.org/sound/cards/audiophile-usb.html

## Descriptor findings from a real unit (bcdDevice 1.01, macOS 12.7.6)

- The configuration descriptor is 1106 bytes long and has 7 interfaces:
  - 0: DFU (class fe/01)
  - 1: AudioControl
  - 2: analog out, EP 0x03
  - 3: digital out, EP 0x04; alt 6 is AC3 (wFormatTag 0x2001)
  - 4: analog in, EP 0x85
  - 5: digital in, EP 0x86; alts 1, 2 and 4 only
  - 6: MIDI, with EP 0x81 interrupt IN (16 bytes), 0x82 bulk IN (16) and 0x02 bulk OUT (16)
- In **alts 2 and 4** (the S/PDIF-in modes) the OUT endpoints are **asynchronous**. Each has
  a 3-byte (10.14) feedback endpoint: 0x83 for interface 2, 0x84 for interface 3. All other
  OUT alts are adaptive. This driver doesn't read the feedback endpoint yet, so the
  S/PDIF-in modes may drift slowly.
- Alt 5 (16-bit) has a maximum packet size of 192 bytes, which is exactly 48 stereo frames.
  The +1-frame drift correction can't fit at 48 kHz, so the stream engine clamps the
  packet.
- macOS's AppleUSBAudio (416.2) matches interfaces 1–5 and opens them exclusively, so
  `USBInterfaceOpenSeize` fails. Interface 6 (MIDI) is left unclaimed.
- On 12.7, `IOServiceMatching("IOUSBHostDevice")` with plain `idVendor` keys returns
  nothing. Use `kIOPropertyMatchKey`.
