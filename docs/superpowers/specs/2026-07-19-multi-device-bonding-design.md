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

Two mechanics to pin down while spiking, both load-bearing:

- **How to program a specific local address.** `setOwnAddrType(BLE_OWN_ADDR_RANDOM)`
  sets the *type*, but NimBLE-Arduino wraps no "set my static address" call —
  confirmed by grep, there is no `ble_hs_id_set_rnd` anywhere in the library.
  The raw NimBLE host call has to be made directly against the bundled stack,
  and the exact ordering relative to `init()` needs to be established
  empirically. The whole architecture rests on this working.
- **Disconnect the active peer cleanly before `deinit`,** rather than pulling
  the stack out from under a live connection.

**If the spike fails, fall back to the whitelist architecture** and record why
in STATUS.md. Do not push on with a mechanism the hardware rejected.

Verification requires two BLE hosts. One device cannot test this feature — a
single host that reconnects proves nothing about slot isolation.

## Spike results — PASSED (2026-07-19)

Run on real hardware by the user, two phones, from branch `spike/slot-switch`
(`src/slot_switch_spike.cpp`). All four switch steps above passed: both hosts
bonded, and switching in both directions typed on the right device **with no
re-pairing prompt**. The per-slot identity architecture is confirmed; the
whitelist fallback is not needed.

**The mechanics, now settled:**

- `ble_hs_id_set_rnd` is called **after** `NimBLEDevice::init()` and returns
  `rc=0`. The ordering works because `init()` ends with
  `while (!m_synced) taskYIELD();`, so it does not return until the controller
  has synced. Do not "fix" this ordering.
- The include is `nimble/nimble/host/include/host/ble_hs_id.h`, guarded on
  `CONFIG_NIMBLE_CPP_IDF`. `<host/ble_hs.h>` does not exist in this build.
- `NimBLEDevice::deinit(true)` **preserves bonds** across the switch and across
  a reboot. No need to fall back to `deinit(false)`.
- `setOwnAddrType(BLE_OWN_ADDR_RANDOM)` stayed in and the switch worked.

**The bug the spike actually caught — this is the load-bearing finding for Task 5.**

The first spike build panicked with `CORRUPT HEAP` inside `~NimBLEServer`, and
separately with `InstrFetchProhibited` (PC=0) on the GAP disconnect path.
Symbolized against the flashed ELF, the cause was a **double free**:

```
loop() -> switchTo -> deinit(true) -> NimBLEServer::~NimBLEServer()
  -> non-virtual thunk to BleKeyboard::~BleKeyboard() -> operator delete
```

`BleKeyboard` derives from `BLEServerCallbacks` and registers itself with
`pServer->setCallbacks(this)` (`BleKeyboard.cpp:108`).
`NimBLEServer::setCallbacks` defaults `m_deleteCallbacks = true`
(`NimBLEServer.cpp:49`), so `~NimBLEServer` runs `delete m_pServerCallbacks`.
**NimBLE owns the `BleKeyboard` object.** The spike deleted it as well.

**Task 5 must handle this, and its version is worse.** `BleHidSink` holds
`BleKeyboard kb_` *by value*, and `main.cpp` instantiates `BleHidSink sink` at
file scope — so `kb_` is in `.bss`, never `malloc`'d. This is inert today only
because the firmware never calls `deinit()`. The moment slot switching
introduces `deinit(true)`, `~NimBLEServer` will call `delete` on a static
address.

Two ways out, and Task 5 must pick one deliberately:

1. **Heap pointer — the model the spike actually validated.** Make `kb_` a
   `BleKeyboard *`, `new` it per switch, and let NimBLE free it in
   `~NimBLEServer`. Proven on hardware across a full switch test. Changes the
   sink's shape.
2. **Keep it by value and disown it.** After `kb_.begin()`, re-register with
   `NimBLEDevice::getServer()->setCallbacks(&kb_, false)` to clear
   `m_deleteCallbacks`. Keeps the current API. **Untested — this is a proposal,
   not a spike result.**

Note what does *not* separate them: the leak. `BleKeyboard` declares **no
destructor** (the one in the backtrace is implicit) and the library contains no
`delete` at all, so the `BLEHIDDevice` that `begin()` allocates is never freed
under either option. Option 1 does not reclaim it by "running the destructor" —
there is nothing in the destructor to run. The services themselves are freed by
`~NimBLEServer` via `m_svcVec`; what leaks is the `BLEHIDDevice` wrapper, in
both cases. **This is why the unmeasured heap delta matters** — it decides
whether either option is viable long-term, or whether the library needs
vendoring.

Also settled: `BleKeyboard` registers itself as *two* callbacks — server
(`BleKeyboard.cpp:108`) and characteristic (line 115). Only the server path
deletes; `~NimBLEServer` destroys characteristics before deleting the server
callback, so if the characteristic also owned it the fixed spike would have
double-freed on the first switch. It did not, across the whole test. Clearing
the server-side flag is therefore sufficient. This also retroactively explains
the `InstrFetchProhibited` (PC=0) crashes: fallout from the already-corrupted
heap, which disappeared after a full `pio run -t erase`.

**Not established by this spike — do not treat as proven:**

- **Heap leaks per switch cycle, and the trend is not yet characterised.**
  Three measured cycles: `-20`, `-48`, `-60` bytes (285112 → 284984, ~43
  avg). It leaks — that much is settled, and it matches the `BLEHIDDevice`
  that `begin()` allocates and nothing frees. What is NOT settled is whether
  the per-cycle cost is converging on a constant (~60 b/cycle → roughly 4700
  switches of runway, survivable) or climbing because the leaked wrapper is
  fragmenting the pool (fatal much sooner than the byte count suggests, and
  invisible to `getFreeHeap()`). Three samples cannot separate these.
  Resolve before Task 5 commits to a teardown model: run ten cycles and see
  whether the deltas flatten; if ambiguous, log `ESP.getMaxAllocHeap()`
  alongside `getFreeHeap()` — free heap sinking slowly while max-alloc sinks
  fast is the fragmentation signature. If it is fragmentation, neither
  ownership option in the section above saves it and the keyboard library
  needs vendoring with a real destructor.
- **Step 4a passed** (observed, not inferred): the address seen on air matched
  the logged `target address` and was stable across runs. So RPA privacy is NOT
  overriding the programmed static address, and
  `setOwnAddrType(BLE_OWN_ADDR_RANDOM)` stays in Task 5 as written — the
  fallbacks contemplated in the spike sketch's comment (dropping the call, or
  `useNRPA=false`) are not needed. This was the silent-failure risk in the
  design; it is closed.
- **Only 2 slots were exercised.** Branch `spike/slot-switch` predates Task 2
  and lacks `-DCONFIG_BT_NIMBLE_MAX_BONDS=4`; NimBLE's default is 3. That 4
  slots work needs its own check once Task 5 lands.

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
- **Bonds** live in NimBLE's own NVS namespace, and the keys themselves are
  never duplicated here.
- **A slot → peer-identity-address map persists in NVS.** This is not a
  redundant copy of the bond store — it is the only record of the association,
  because NimBLE does not keep one. Verified in the vendored source:
  `getBondedAddress(index)` wraps `ble_store_util_bonded_peers()`, which returns
  a flat list of **peer** identity addresses in store order. Nothing in a bond
  record names the local identity it was created against, so store order has no
  relationship to slot number, and `getNumBonds()` is a total rather than a
  per-slot fact.

  Without this map, three things are impossible: `U <slot>` cannot pick which
  bond to hand `deleteBond(peerAddr)`; the `bonded`/`empty` field in the `S` and
  `P` replies cannot be computed; and the whitelist fallback architecture cannot
  populate its whitelist. The map is written when authentication completes, from
  the connection's `getIdAddress()`.

  Note that `S <slot>` — selection itself — does *not* need it. Advertising as
  identity *i* is enough; the host that bonded to identity *i* recognises the
  address and reconnects with its stored LTK. This is why the spike below cannot
  surface the gap: it exercises only selection.
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

The sink also observes authentication-complete in order to record the peer
identity address into the slot map. That is BLE-side bookkeeping and belongs
there, not in `main.cpp`.

Given that, the slot logic — address derivation, active-slot persistence, and
the slot → peer map — should go in its own small unit beside the sink rather
than inside it. The sink's job is reports; it should not also become the place
NVS lives. Address derivation stays a pure function.

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
