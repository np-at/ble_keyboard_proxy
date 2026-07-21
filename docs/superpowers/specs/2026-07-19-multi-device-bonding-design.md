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

## Spike results — switch gate PASSED (2026-07-19), reconnect BLOCKED (2026-07-20)

Run on real hardware by the user, two phones, from branch `spike/slot-switch`
(`src/slot_switch_spike.cpp`). All four switch steps above passed: both hosts
bonded, and switching in both directions typed on the right device **with no
re-pairing prompt**. The per-slot identity architecture is confirmed; the
whitelist fallback is not needed.

> **UPDATE 2026-07-20 — a blocker was found underneath the switch gate.** A bonded
> device **cannot reconnect into a working typing link** under the per-slot
> static-random address setup (both the `deinit` switch *and* the Option 3 hot
> swap fail the same way; a plain phone-side BT cycle with no server rebuild also
> fails). The shipping `main` firmware reconnects and types fine, so this is
> **not** product-wide — it is isolated to the spike's address config. Blocks
> Task 5. Full analysis and the diagnostic-first next step are in **"Reconnect
> blocker (2026-07-20)"** below. The 2026-07-19 findings in this section remain
> accurate about the *switch mechanism*; they simply did not test bonded reconnect.

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
both cases. The measured leak is a constant 60 bytes per switch (see the
results below), which both options carry equally and which is small enough to
ship.

### Option 3: hot address swap — no teardown at all (UNSPIKED)

Both options above assume the switch requires `deinit(true)`. It does not.
`ble_hs_id_set_rnd` (`ble_hs_id.c:152`) has no dependency on init/deinit state:
it validates the address bits, sends HCI `LE_Set_Random_Address`, and `memcpy`s
into `ble_hs_id_rnd`. Nothing more. So the switch can be:

```
disconnect active peers
NimBLEDevice::stopAdvertising()
ble_hs_id_set_rnd(newSlotAddr)
NimBLEDevice::startAdvertising()
```

The stack, GATT server, `BLEHIDDevice` and `BleKeyboard` all stay alive.
Nothing is destroyed or reallocated, which means **the 60-byte leak goes to
zero** (there is no second `begin()` to allocate it) and **the ownership hazard
becomes unreachable** (no `~NimBLEServer`, so nothing ever deletes the callbacks
object — `kb_` can stay by value). Switching is also faster and GATT handles
stay stable.

**Not verified on hardware. Two real unknowns:** the Bluetooth spec has the
controller reject `LE_Set_Random_Address` with *Command Disallowed* while
advertising/scanning/initiating is enabled — hence stopping advertising first,
but whether the ESP32-S3 controller accepts it with a live GATT server is
empirical, and `rc` will say. Second, whether bonded hosts still reconnect
after a hot swap: the mechanism depends on the peer's stored LTK surviving a
change of *our* identity. Spike it as an extra command alongside `0`-`3` so
both paths can be A/B'd in one build; the `deinit` path is validated and stays
the fallback.

Also settled: `BleKeyboard` registers itself as *two* callbacks — server
(`BleKeyboard.cpp:108`) and characteristic (line 115). Only the server path
deletes; `~NimBLEServer` destroys characteristics before deleting the server
callback, so if the characteristic also owned it the fixed spike would have
double-freed on the first switch. It did not, across the whole test. Clearing
the server-side flag is therefore sufficient. This also retroactively explains
the `InstrFetchProhibited` (PC=0) crashes: fallout from the already-corrupted
heap, which disappeared after a full `pio run -t erase`.

**Not established by this spike — do not treat as proven:**

- **Heap: a constant 60-byte leak per switch. Characterised over 21 switches,
  now CLOSED.** Measure at the same point in every cycle — the `slot N up,
  heap=` line. The `heap before/after ... delta=` figures the spike prints are
  **noise**: those two samples straddle transient connection and advertising
  allocations, and they ranged `-4` to `-72` while the real per-cycle cost never
  moved. At the stable sampling point: `-60` on 20 of 21 switches, one `-68`;
  first-half mean `-60.8`, second-half mean `-60.0`; 1268 bytes over 21
  switches.

  **No acceleration, so this is not fragmentation** — it is a fixed allocation
  never freed, matching the `BLEHIDDevice` that `begin()` news on each rebuild.
  Therefore `ESP.getMaxAllocHeap()` instrumentation is unnecessary and the
  keyboard library does **not** need vendoring.

  Runway: ~4700 switches to bare exhaustion, realistically ~800–1600 before
  BLE-stack headroom gets tight. RAM-only — a reboot resets it fully. Shippable
  as-is; document it in `STATUS.md` during Task 8.
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

### Reconnect blocker (2026-07-20) — BLOCKS Task 5

The switch gate above tested *switching between two already-connected hosts*. It
did **not** test a bonded host **reconnecting from cold** with the slot address
in place. That path is broken.

**Hardware results (iOS is the reliable oracle; Android HID never renders):**

| Test | Result |
|---|---|
| Hot-swap `set_rnd` rc / address change / heap / bonds | rc=0, address changes, delta=0, bonds=2 — mechanically perfect |
| Hot-swap: iOS reconnect | **FAIL** |
| Deinit-switch: iOS reconnect (clean bench) | **FAIL** |
| Full reboot: iOS reconnect | **"connects" in iOS UI but does NOT type** (hollow) |
| Phone-side BT cycle to the *same running server* (no GATT rebuild) | **FAIL** |
| Fresh pair (any slot), iOS: typing | **WORKS** |
| **`main` firmware: fresh pair types, then BT-cycle reconnect types** | **BOTH WORK** |

So both switch mechanisms are moot until reconnect is fixed — the bug sits
*underneath* the switch-mechanism choice. Option 3's zero-leak advantage is
irrelevant while reconnect is broken.

**Source-grounded analysis (vendored NimBLE-Arduino 1.4.3 — the compiled code):**

- **The only runtime delta between `main` (reconnects) and the spike (fails) is
  the local address**: `ble_hs_id_set_rnd()` + `own_addr_type = RANDOM`. Verified
  identical otherwise — `sm_our_key_dist`/`sm_their_key_dist` are both `ENC|ID`
  for main and spike (`NimBLEDevice.cpp:895-896`; the spike's `setOwnAddrType`
  re-sets the same value), and peer-IRK resolving-list population
  (`ble_store.c:241`, `ble_hs_misc.c:130`) is ungated on address type.
- **Two earlier hypotheses are refuted by source:**
  1. *"RPA on air instead of the static address"* — ruled out. Host-based privacy
     is compiled out on the S3 (`CONFIG_BT_NIMBLE_HOST_BASED_PRIVACY` undefined →
     0), so `setOwnAddrType(RANDOM)` never calls `ble_hs_pvcy_rpa_config`. The
     scanner already confirmed the static address on air (Step 4a above).
  2. *"The spike distributes the public identity, not the static-random slot
     address"* — contradicted. `IDENTITY_ADDR_INFO` (`ble_sm.c:2194`) sends
     `our_id_addr`, which for RANDOM own-addr-type is `ble_hs_id_rnd` (the address
     just programmed); the privacy-override branch (`ble_sm.c:2181`) is compiled
     out. Distributed identity == advertised address.
- **The critical path:** on an iOS SC reconnect the peripheral must resolve iOS's
  rotating RPA to the bonded identity, because the LTK store lookup keys on the
  resolved peer identity (`ble_sm_ltk_req_rx` → `ble_sm_retrieve_ltk`,
  `ble_sm.c:1439`; ediv/rand are 0 under SC). A failed resolution → LTK not found
  → encryption never completes → "connects but doesn't type."
- **Honest gap:** resolution and LTK-lookup paths read *identically* in source for
  main and spike, so source alone cannot say whether the spike breaks at
  resolution, LTK lookup, or CCCD restore. It is a controller-level interaction
  with the static-random address that only on-device logs can localize.

**Next step (no blind hardware cycles): a diagnostic-first flash.** The spike now
carries `dumpConnInfo`/`pollConnState` (added 2026-07-20), which print per-peer
`ota`/`id`/`bonded`/`enc` **on every connection state change** via the public
`NimBLEConnInfo` API (no library patching). On a reconnect the `id`-vs-`ota`
address pair discriminates the failing sub-step — resolution vs LTK vs CCCD — and
that picks the fix. See the reconnect-research note for the decode table.

**Multi-device landmine for the 4-slot goal (independent of the reconnect fix):**
all ESP32 NimBLE devices ship the **same default local IRK**
([h2zero/NimBLE-Arduino #539](https://github.com/h2zero/NimBLE-Arduino/issues/539)).
Per-slot bonds on one chip already use *distinct* static-random addresses; give
each slot a **distinct local IRK** too (`ble_hs_pvcy_set_our_irk`, set before
bonds load) to remove cross-slot identity ambiguity on the phone. Worth doing
regardless of what the diagnostic shows.

**Android (separate, untriaged):** Android never types even on a fresh pair
(soft keyboard *is* suppressed → recognized as a keyboard, but no characters
render). An Android-only BLE-HID target-compat problem, independent of
multi-device.

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
