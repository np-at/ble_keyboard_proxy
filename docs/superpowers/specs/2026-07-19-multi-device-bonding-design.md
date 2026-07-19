# Multi-device bonding — design

Hold BLE bonds with up to four devices at once and switch the active one on
demand, without re-pairing. Today a single bond is supported and changing
devices means re-pairing, which defeats the purpose of a proxy keyboard.

Scope: **N bonds, one active at a time, switch on command.** Not two
simultaneous connections — that is a different and much harder problem, and it
is not what this feature is for.

## Chosen architecture: distinct identity address per slot

Four local static-random addresses, one bond each. Switching means: stop
advertising, tear the BLE stack down, set a new own-address, bring it back up,
advertise. Each host only ever sees the identity it bonded to, so the host you
switched *away* from cannot reconnect — it can no longer see an address it
recognises. This is the model real multi-host keyboards ship (ZMK, Logitech
Easy-Switch).

### The alternative, and why it lost

**Single identity + whitelist-filtered advertising.** One MAC, N bonds, switch
by setting the whitelist to slot *i*'s peer and calling
`advertising->setScanFilter(true, true)`. Cheaper to write — no stack teardown,
`BleKeyboard` stays a by-value member, roughly fifty lines.

It loses on failure mode, not on effort. The host you switch away from keeps
trying to reconnect, and the whitelist is the only thing stopping it. That
depends on the controller resolving the peer's RPA against the resolving list,
which is historically unreliable on ESP32. The resulting bug would be
intermittent and host-dependent — precisely the class of problem that cost this
project several rounds of debugging when a dead BLE link looked healthy.
Per-slot identity has a higher upfront cost that is mechanical and visible.

This remains the documented fallback if the spike below fails.

## Spike gate — do this first

Before any of the design below is built, a throwaway sketch must demonstrate on
real hardware:

1. Bond host A as slot 0. Type. Confirm on the device.
2. Switch to slot 1, bond host B. Type. Confirm on the device.
3. Switch back to slot 0. Type **without re-pairing**. Confirm on the device.
4. Switch to slot 1 again. Type without re-pairing. Confirm.

Two measurements to take while it runs, because both are live risks:

- **Heap delta per switch cycle.** `BleKeyboard::begin()` calls `new` on its
  `BLEHIDDevice` and never frees it; `BleKeyboard::end()` is an empty stub. If
  each switch leaks, the board dies after some number of switches. Log
  `ESP.getFreeHeap()` before and after each cycle. A steady leak means the
  rebuild path needs explicit cleanup, or the library needs vendoring.
- **Does `NimBLEDevice::deinit(true)` preserve bonds?** `clearAll` frees the
  server and scan objects. It must not touch the NimBLE bond records in NVS. If
  it does, use `deinit(false)` and account for whatever it leaves behind.

**If the spike fails, fall back to the whitelist architecture** and record why
in STATUS.md. Do not push on with a mechanism the hardware rejected.

Verification requires two BLE hosts. One device cannot test this feature — a
single host that reconnects proves nothing about slot isolation.

## Identity addresses

Derived from the chip's efuse base MAC:

- Top two bits of the most significant byte forced to `0b11`, which BLE requires
  for a static random address.
- Slot index mixed into the least significant byte.

Stable across reflashes, unique per board, nothing to store or configure. The
derivation is a pure function so it can be reasoned about and logged.

## State and persistence

- **Active slot** persists in NVS (Preferences), so a reboot returns to the same
  host rather than silently landing on slot 0.
- **Bonds** live in NimBLE's own NVS namespace. Nothing here duplicates them —
  a second source of truth about which slots are bonded would drift. Bond
  presence is read from `NimBLEDevice::getNumBonds()`/`getBondedAddress()`.
- `CONFIG_BT_NIMBLE_MAX_BONDS=4` as a build flag in `platformio.ini`, commented
  with why it is not the default 3.

## Wire protocol

Three new verbs, same newline-terminated hex-ish shape as the existing ones.
Slot is a single digit, 0–3.

| Command | Meaning | Reply |
|---|---|---|
| `S <slot>` | Select the active slot | `OK slot 2 bonded advertising` |
| `B <slot>` | Clear that slot's bond and advertise openly to pair | `OK pairing 2` |
| `U <slot>` | Forget that slot's bond | `OK cleared 2` |

`P` extends to report the active slot alongside connection state.

**These reply `OK`, not `SENT`,** and the distinction is deliberate. `K`/`C`
reply `SENT` because a HID input report is a fire-and-forget GATT notification —
the firmware cannot know the host applied it. A slot switch is different: the
firmware genuinely knows whether it changed its own identity and restarted
advertising, so `OK` is honest about a local action. What `OK` does **not**
claim is that the host reconnected. That is still only observable on the device.

Pairing is explicit per slot rather than automatic on an empty slot, so a stray
device cannot silently claim a slot.

## Layering

The existing seam holds. No layer learns about another.

| Layer | Change |
|---|---|
| `hid_report.h` | `CommandKind::SelectSlot`/`PairSlot`/`ForgetSlot`, each carrying a `uint8_t slot`. |
| `serial_proto` | Parses the three verbs, range-checks slot 0–3. Still knows nothing about BLE. |
| `ble_hid_sink` | Gains `selectSlot`/`pairSlot`/`forgetSlot`/`activeSlot`. Owns identity switching. Still knows nothing about serial. |
| `main.cpp` | Routes the new commands. Stays thin. |

**One forced change:** `BleKeyboard kb_` becomes a pointer. Switching identity
requires destroying and rebuilding the keyboard object, which a by-value member
cannot express. This is the main structural cost of the chosen architecture.

If `ble_hid_sink.cpp` grows past roughly 150 lines, split the slot logic —
address derivation and active-slot persistence — into its own small unit beside
it. Do not let the sink become the place everything lands.

## Host side

`host/proxy_term.py` gains slot awareness:

- `Ctrl-]` then `1`–`4` selects a slot.
- `Ctrl-]` then `!` then `1`–`4` enters pairing mode for that slot.
- `Ctrl-]` then `?` lists slots, their bound target, and which is active.

Each slot remembers which `TARGETS` keymap it uses, persisted in a small JSON
file beside the script. Switching to the iPad slot swaps to iPad chords
automatically — the keymap follows the device, which is the whole point of
having slots.

Slot resolution and the JSON round-trip are pure functions, in the same style as
the existing key resolution and escape decoding, so `test_proxy_term.py` covers
them. A missing or malformed JSON file falls back to defaults rather than
failing to start.

## Testing and verification

- **Host tests** cover the pure layer: slot resolution, keymap-per-slot lookup,
  JSON round-trip and its fallback, and the encoding of the three new commands.
  No hardware.
- **Firmware** is verified only by a human watching two devices. Per this
  repo's discipline, a clean serial reply is not evidence a slot switch reached
  anything.
- **STATUS.md** gains a slot table using the same VERIFIED/UNVERIFIED marking as
  `TARGETS`. Every slot-related claim starts UNVERIFIED and is promoted only
  after someone watched it work.

## Out of scope

- Two simultaneous connections.
- Slot names or per-slot device metadata in firmware. The host owns the
  slot → target mapping; firmware knows only numbers.
- Any change to the `K`/`C`/`R` report path.
