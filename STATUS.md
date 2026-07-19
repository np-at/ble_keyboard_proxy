# BLE Keyboard Proxy — status and roadmap

Canonical state of this repository. Update it when the facts below change.

An ESP32-S3 that presents itself to a phone or tablet as a Bluetooth keyboard and
types whatever a host computer sends it. The host link is USB serial today; the
long-term goal is system-wide keyboard capture on macOS.

## Hardware and environment

- **Board:** ESP32-S3-DevKitC-1-N16R8 (16MB flash, 8MB PSRAM).
- **Plug into the UART USB-C port**, not the native USB port. The UART bridge is a
  CH343 (`1A86:55D3`) and has working auto-reset. esptool cannot reset the native
  port (`303A:4001`) into download mode at all — flashing there simply fails.
  Confirm with `pio device list` before flashing.
- **pyserial lives only in the PlatformIO venv.** Use `~/.platformio/penv/bin/python`
  for the host scripts, or install pyserial into your system python.
- **The serial port takes one owner.** The PlatformIO monitor and any host script
  cannot both hold it; the symptom looks like the firmware hanging, not a conflict.
  Close the monitor first.

```bash
pio run                 # build (default env only — see platformio.ini)
pio run -t upload       # flash (UART port)
pio run -t erase        # full chip erase — invalidates bonds, forces re-pair
pio device monitor      # serial monitor (close before running host scripts)
pio test -e native      # native unit tests for pure logic — no board needed

~/.platformio/penv/bin/python host/test_proxy_term.py             # host tests
~/.platformio/penv/bin/python host/proxy_term.py --target iphone  # interactive
```

A healthy build is roughly `RAM 9.1% / Flash 7.6%`. A much larger flash figure
(~885KB) means `-DUSE_NIMBLE` was lost — see Constraints.

## Layout

| Path | Role |
|---|---|
| `platformio.ini` | Board config. Commented with the *why* for each non-obvious setting. |
| `src/main.cpp` | `setup()`/`loop()`; pumps serial → parser → sink. |
| `src/hid_report.h` | `HidReport`/`ConsumerReport`/`Command` — the transport-agnostic seam. |
| `src/serial_proto.h/.cpp` | Non-blocking hex line parser. No BLE knowledge. |
| `src/ble_hid_sink.h/.cpp` | Wraps `BleKeyboard`. No serial knowledge. |
| `src/slot_address.h` | Derives a per-bond-slot BLE identity address from the chip's base MAC. Header-only, no Arduino/BLE includes — natively testable. |
| `host/proxy_term.py` | Terminal → BLE proxy. Raw-mode TTY loop, `--text`, `--probe`. |
| `host/test_proxy_term.py` | Tests for the pure host layer. No hardware needed. |

`HidReport` is a deliberate seam: swapping the producer (serial today, a USB-OTG
host keyboard or a macOS event tap later) must not touch `ble_hid_sink`.

## Wire protocol

Newline-terminated ASCII over USB serial at 115200. Hex bytes, one or two digits.

| Command | Meaning | Reply |
|---|---|---|
| `K <mod> <k1..k6>` | Full keyboard state — 7 bytes | `SENT` |
| `C <b0> <b1>` | Consumer/media bitmask — 2 bytes | `SENT` |
| `R` | Release everything | `SENT` |
| `P` | Ping / connection state | `OK connected` or similar |

**Reports are state, not events.** One report lists every key currently held, so
producers emit the complete current state on each change; hold, repeat and chording
need no down/up encoding. A tap is therefore two reports: the key held, then nothing
held.

**`K` and `C` reply `SENT`, never `OK`.** HID input reports are fire-and-forget GATT
notifications, so the firmware can confirm only that it handed the report to the BLE
stack — never that the device applied it. Treating `SENT` as delivery is what made a
completely dead BLE link look healthy for several rounds of debugging. **Anything
involving what the device actually did must be confirmed by a human watching it.**

## Constraints worth knowing before changing things

1. **NimBLE is required, not an optimization.** Bluedroid cannot serve HID to a phone
   at all: it completes bonding, then fails `BTM_GetSecurityFlags` on the peer's
   resolvable private address and rejects every HID access with
   `GATT_INSUF_AUTHENTICATION`. Erasing NVS does not help. Do not remove
   `-DUSE_NIMBLE`. NimBLE is pinned to `^1.4.3` because the library's `USE_NIMBLE`
   path targets the 1.x API and will not compile against 2.x.
2. **The BLE keyboard library is pinned to commit `b7aaf9b`,** not a tag — tag
   `0.3.2` does not exist upstream.
3. **`ARDUINO_USB_CDC_ON_BOOT=0`** because the board is flashed over UART. With it
   set to 1 the ROM banner still appears but application output never does, which
   reads as dead firmware rather than a wrong flag. If the cable moves to the native
   USB port, this flag moves with it.
4. **Consumer keys are limited to the 16 baked into the library's HID descriptor** —
   not the 16-bit consumer usage space. `b0`/`b1` are the `KEY_MEDIA_*` masks. Play/
   pause is `C 08 00`, *not* `C 00 CD`. Anything outside those 16 needs a vendored
   library with an extended descriptor.
5. **Phone-side text assistance corrupts test results** and masquerades as firmware
   bugs — auto-capitalization turned a sent `a` into `A`, auto-punctuation turned
   `x y z w` into `X. Y. Z. W`. Disable it before testing text.

## Verified behaviour

Everything here was confirmed by a human watching the device.

- Pairs as a BLE keyboard; typing, shift chords, arrows and media keys all work.
- Shift+Tab works (terminals send it as CSI Z, `ESC [ Z`).
- `host/proxy_term.py` interactive mode: full pass, including leader commands,
  help, quit and terminal restore.
- Host tests: 12/12 passing.

### Device commands, per target

`Ctrl-]` is a leader key in interactive mode: `h` Home, `b` Back, `a` app switcher,
`s` Spotlight/search, `q` quit, `?` list.

| Command | iPhone | iPad | Android |
|---|---|---|---|
| Home | AC Home — **works** | Cmd+H — untested | AC Home — untested |
| Spotlight | Cmd+Space — **works** | Cmd+Space — untested | not mapped |
| Back | **unavailable** | Cmd+[ — untested | AC Back — untested |
| App switcher | **unavailable** | Cmd+Tab — untested | Meta+Tab — untested |

**iPhone has no Back and no app switcher over BLE HID.** iOS ignores AC Back even
though AC Home works over the identical mechanism, and `Cmd+[` / `Cmd+Tab` are
iPad-only. `Cmd+Space` working proves the GUI modifier reaches the device, so these
are OS limitations, not bugs here — do not try to "fix" them with other Cmd chords.
Those two commands report why instead of silently sending a dead chord.

**iPad and Android mappings are unverified guesses.** Predictions about which chords
an OS honours have already proven unreliable, so treat them as hypotheses. Test with
`--probe`, which sends one raw action and exits:

```bash
~/.platformio/penv/bin/python host/proxy_term.py --probe K:08:2B   # Cmd/Meta+Tab
~/.platformio/penv/bin/python host/proxy_term.py --probe C:00:20   # AC Back
```

## Roadmap

**Done — M1: firmware.** Pairs and types, verified on hardware.

**Done — M1.5: terminal proxy.** `host/proxy_term.py`, verified end-to-end.

**Next — multi-device bonding.** Hold bonds with several devices at once and switch
the active target on demand. Today a single bond is supported and changing devices
means re-pairing, which defeats the point of the thing. Firmware work: NimBLE bond-
slot management plus a serial verb to select the peer, then a host command to drive
it. Switching device should probably switch the `TARGETS` keymap with it.
Address derivation (`src/slot_address.h`, `[env:native]` unit tests) is done; bond-
slot management and the serial verb are not started.

**Then — M2: macOS system-wide capture.** A CGEventTap daemon replacing the terminal
as the producer, so keystrokes are proxied from anywhere rather than only from a
terminal window. Needs Accessibility permission, a ~100-entry virtual-keycode → HID
usage table, and a consume-vs-mirror decision (mirror recommended as the default).
This is where full-state reports pay off: an event tap can express genuine key hold
and auto-repeat, which a raw-mode terminal fundamentally cannot — it delivers
characters, not key up/down events.

**Unscheduled.** Extended consumer descriptor (vendored library) if more than the 16
media keys are ever needed; USB-OTG host input as a second producer.
