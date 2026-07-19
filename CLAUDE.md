# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `STATUS.md` first.** It is the canonical record of repo state, hardware
constraints, the wire protocol, what has been verified on real hardware, and the
roadmap. This file covers only how to work in the repo; `STATUS.md` covers what is
true about it. Keep `STATUS.md` current when facts change.

## Commands

```bash
pio run                 # build
pio run -t upload       # flash
pio run -t erase        # full chip erase — invalidates BLE bonds, forces re-pair
pio device monitor      # serial monitor

# Host tests — no hardware needed, run these freely
~/.platformio/penv/bin/python host/test_proxy_term.py            # all
~/.platformio/penv/bin/python host/test_proxy_term.py escape     # substring filter

# Host proxy — needs the board attached
~/.platformio/penv/bin/python host/proxy_term.py --text 'hello'  # non-interactive
~/.platformio/penv/bin/python host/proxy_term.py --probe C:80:00 # one raw action
~/.platformio/penv/bin/python host/proxy_term.py --target iphone # interactive TTY
```

Use `~/.platformio/penv/bin/python`: pyserial is installed only in the PlatformIO
venv, not in system python.

A healthy build is about `RAM 9.1% / Flash 7.6%`. A flash figure near 885KB means
`-DUSE_NIMBLE` was dropped — that flag is load-bearing, see `platformio.ini`.

## Architecture

```
computer --USB CDC serial--> ESP32-S3 --BLE HID--> phone/tablet
```

Firmware is three layers with a deliberate seam between them:

- `serial_proto` parses hex lines into a `Command`. **Knows nothing about BLE.**
- `hid_report.h` defines `HidReport`/`ConsumerReport`/`Command` — the seam.
- `ble_hid_sink` wraps the BLE keyboard library. **Knows nothing about serial.**
- `main.cpp` is the only place the two meet: it pumps serial → parser → sink.

Preserve that separation. The producer is expected to change (serial today, a macOS
CGEventTap daemon or a USB-OTG host keyboard later) and swapping it must not require
touching `ble_hid_sink`. Anything that makes the sink aware of serial, or the parser
aware of BLE, is a regression regardless of how convenient it is.

**HID reports are state, not events.** One report describes every key currently held,
so producers emit the complete current state on every change. Hold, auto-repeat and
chording fall out for free with no down/up encoding. A tap is two reports: key held,
then nothing held. Do not add event-style down/up messages — it would fight the
transport rather than extend it.

The host side (`host/proxy_term.py`) mirrors this: pure functions for key resolution
and escape decoding, a thin `Link` class for the serial protocol. The pure layer is
what `host/test_proxy_term.py` covers, and it is where a wrong usage code would
otherwise be silent.

## Verification discipline

This matters more here than in most repos, because the failure mode is invisible from
the machine you are working on.

**`SENT` does not mean delivered.** HID input reports are fire-and-forget GATT
notifications, so the firmware confirms only that it handed the report to the BLE
stack. A completely dead BLE link looked healthy from the firmware side for several
rounds of debugging. Neither `SENT`, nor a clean exit, nor passing tests are evidence
that a keystroke or command reached the device.

Practical consequences:

- **Never claim a device-facing change works without a human confirming on the
  device.** Ask, and wait for the answer.
- **Do not guess which chords an OS honours over BLE HID.** Several confident
  predictions in this project have been wrong. Mappings in `TARGETS` are marked
  VERIFIED or UNVERIFIED; use `--probe KIND:BYTE:BYTE` to test candidates and only
  promote an entry to VERIFIED once someone has watched it work.
- **Phone-side text assistance corrupts results** and mimics firmware bugs
  (auto-capitalization, auto-punctuation). Rule it out before debugging.
- Where something genuinely cannot work on a target, make it say so — `TARGETS`
  supports an `("X", reason, "")` action. A command that silently does nothing is the
  same failure as the firmware answering `OK` for an undelivered report.

## Traps

- **Flash over the UART port** (CH343, `1A86:55D3`), not the native USB port
  (`303A:4001`), which esptool cannot reset into download mode. Check with
  `pio device list`.
- **The serial port takes one owner.** The PlatformIO monitor and any host script
  cannot both hold it. The symptom looks like hung firmware, not a conflict — close
  the monitor first.
- **Consumer/media keys are limited to the 16 in the library's HID descriptor**, not
  the 16-bit consumer usage space. Play/pause is `C 08 00`, not `C 00 CD`.
- `platformio.ini` comments explain *why* each non-obvious setting exists, several
  established empirically at real cost. Read them before changing build flags or
  loosening the pinned library commits.
