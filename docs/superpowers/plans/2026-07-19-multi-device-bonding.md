# Multi-device bonding — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hold BLE bonds with up to four devices at once and switch the active one on command, without re-pairing.

**Architecture:** Each slot gets its own static-random BLE identity address derived from the chip's base MAC. Switching slots tears the NimBLE stack down, programs a new local address, and brings it back up, so the host you switched away from can no longer see an address it recognises. The firmware persists a slot → peer-identity-address map in NVS, because NimBLE's bond store does not record which local identity a bond belongs to.

**Tech Stack:** ESP32-S3 / Arduino / PlatformIO, NimBLE-Arduino 1.4.x, T-vK ESP32-BLE-Keyboard (pinned commit `b7aaf9b`), Python 3 + pyserial on the host.

Spec: `docs/superpowers/specs/2026-07-19-multi-device-bonding-design.md`

## Global Constraints

- **`-DUSE_NIMBLE` is load-bearing.** Bluedroid cannot serve HID to a phone at all. Never remove it. A build whose flash figure approaches 885KB means it was dropped; a healthy build is roughly `RAM 9.1% / Flash 7.6%`.
- **NimBLE-Arduino pinned to `^1.4.3`** — the library's `USE_NIMBLE` path targets the 1.x API and will not compile against 2.x. Do not bump it.
- **ESP32-BLE-Keyboard pinned to commit `b7aaf9bb711a04216e4417f1e2a6b0ee0eaeaf66`.** Tag `0.3.2` does not exist upstream.
- **Flash over the UART port** (CH343, `1A86:55D3`), never the native USB port (`303A:4001`). Check with `pio device list`.
- **The serial port takes one owner.** Close `pio device monitor` before running any host script. The symptom of a conflict looks like hung firmware.
- **Use `~/.platformio/penv/bin/python`** for host scripts. pyserial is not in system python.
- **`K`/`C`/`R` reply `SENT`, never `OK`.** The new slot verbs reply `OK` because a slot switch is a local action the firmware genuinely knows it performed. Do not change the existing replies.
- **Preserve the layer seam.** `serial_proto` must not learn about BLE; `ble_hid_sink` must not learn about serial. `main.cpp` is the only place they meet.
- **Never claim a device-facing change works without a human confirming on the device.** A clean serial reply is not evidence.

---

## Task 1: Hardware spike — prove the switch mechanism (GATE)

This is throwaway code on a throwaway branch. Nothing else in this plan may start until it passes. Its purpose is to answer three questions the rest of the design assumes.

**Files:**
- Create: `spike/slot_switch/slot_switch.ino.cpp` (throwaway; deleted in Step 8)

**Interfaces:**
- Consumes: nothing.
- Produces: a yes/no answer on the architecture, plus the confirmed call sequence for programming a local static-random address, which Task 5 depends on.

- [ ] **Step 1: Create a spike branch**

```bash
git checkout -b spike/slot-switch
mkdir -p spike/slot_switch
```

- [ ] **Step 2: Write the spike sketch**

Point the build at the spike by adding this to `[env:esp32-s3-devkitc-1]` in `platformio.ini`. Revert it in Step 8.

```ini
; TEMPORARY — spike only. Swaps the real firmware out for the spike sketch.
build_src_filter = +<*> -<main.cpp> +<../spike/slot_switch/slot_switch.ino.cpp>
```

There is no `build_src_dir` option in PlatformIO — `build_src_filter` is the real one. Excluding `main.cpp` matters: leaving it in gives duplicate `setup()`/`loop()` symbols at link time.

```cpp
// Throwaway. Answers three questions and is then deleted:
//   1. Does per-slot identity switching work without re-pairing?
//   2. Does the switch leak heap?
//   3. Does deinit(true) preserve bonds?
#include <Arduino.h>
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <host/ble_hs.h>

namespace {

BleKeyboard *kb = nullptr;
uint8_t activeSlot = 0;

// Same derivation Task 2 makes permanent: static random needs the top two bits
// of the MSB set, and slots must differ, so the index goes in the LSB.
void slotAddress(uint8_t slot, uint8_t out[6]) {
  uint8_t base[6];
  esp_read_mac(base, ESP_MAC_BT);
  memcpy(out, base, 6);
  out[0] |= 0xC0;
  out[5] = (uint8_t)((base[5] & 0xFC) | (slot & 0x03));
}

void startSlot(uint8_t slot) {
  activeSlot = slot;

  uint8_t addr[6];
  slotAddress(slot, addr);

  // NimBLE's ble_addr_t is little-endian; our array is MSB-first.
  ble_addr_t rnd;
  rnd.type = BLE_ADDR_RANDOM;
  for (int i = 0; i < 6; i++) rnd.val[i] = addr[5 - i];

  // Set the address after init(), not before: init() ends with
  // `while(!m_synced) taskYIELD();`, so it does not return until the controller
  // has synced and ble_hs_id_set_rnd can succeed. Checked in NimBLEDevice.cpp.
  NimBLEDevice::init("Proxy Keyboard");
  const int rc = ble_hs_id_set_rnd(rnd.val);
  Serial.printf("[spike] ble_hs_id_set_rnd rc=%d\n", rc);

  // THE OPEN QUESTION IS THIS CALL, not the ordering above. With
  // BLE_HOST_BASED_PRIVACY enabled, setOwnAddrType(BLE_OWN_ADDR_RANDOM) also
  // calls ble_hs_pvcy_rpa_config(NIMBLE_HOST_ENABLE_RPA) — which makes the
  // device advertise a *resolvable private address* rather than the static one
  // just programmed. That would defeat per-slot identity entirely, and rc == 0
  // would not reveal it. Step 4a checks the address actually on air.
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

  kb = new BleKeyboard("Proxy Keyboard", "ble_keyboard_proxy", 100);
  kb->begin();

  Serial.printf("[spike] slot %u up, heap=%u, bonds=%d\n",
                slot, ESP.getFreeHeap(), NimBLEDevice::getNumBonds());
}

void switchTo(uint8_t slot) {
  Serial.printf("[spike] heap before switch=%u\n", ESP.getFreeHeap());

  // Disconnect cleanly rather than yanking the stack out from under a live link.
  NimBLEServer *server = NimBLEDevice::getServer();
  if (server != nullptr) {
    for (uint16_t h : server->getPeerDevices()) server->disconnect(h);
    delay(200);
  }

  delete kb;
  kb = nullptr;
  NimBLEDevice::deinit(true);
  delay(200);

  startSlot(slot);
  Serial.printf("[spike] heap after switch=%u\n", ESP.getFreeHeap());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[spike] commands: 0-3 select slot, t type, x erase bonds, h heap");
  startSlot(0);
}

void loop() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c >= '0' && c <= '3') {
      switchTo((uint8_t)(c - '0'));
    } else if (c == 't') {
      if (kb->isConnected()) {
        kb->print("slot");
        kb->print((int)('0' + activeSlot));
        Serial.println("[spike] typed");
      } else {
        Serial.println("[spike] not connected");
      }
    } else if (c == 'x') {
      NimBLEDevice::deleteAllBonds();
      Serial.println("[spike] all bonds erased");
    } else if (c == 'h') {
      Serial.printf("[spike] heap=%u bonds=%d\n",
                    ESP.getFreeHeap(), NimBLEDevice::getNumBonds());
    }
  }
  delay(5);
}
```

- [ ] **Step 3: Build and flash**

```bash
pio run -t upload
```
Expected: build succeeds, upload completes over the CH343 UART port.

- [ ] **Step 4a: Confirm the advertised address is the one we programmed**

Before testing bonds, check what is actually on air. Use a BLE scanner app (nRF Connect, LightBlue) and look at the address "Proxy Keyboard" advertises.

Expected: it matches the address the spike logs for the active slot, and it *changes* when you send `1`. If instead it looks random and differs on every scan, RPA privacy is on and per-slot identity cannot work as written — try `setOwnAddrType(BLE_OWN_ADDR_RANDOM, /*useNRPA=*/false)` with privacy disabled, or drop the `setOwnAddrType` call entirely and see whether the programmed static address is used on its own. Record what worked.

Getting this wrong is silent: bonding would still succeed and hosts would still reconnect, but slot isolation would be an illusion. Do not skip to Step 4 without checking.

- [ ] **Step 4: Run the switch sequence with two hosts**

Open `pio device monitor`. Then, watching both devices:

1. Send `x` to clear any stale bonds.
2. Pair host A with "Proxy Keyboard". Send `t` — **confirm `slot0` appears on host A**.
3. Send `1`. Pair host B. Send `t` — **confirm `slot1` appears on host B**.
4. Send `0`. Wait for host A to reconnect on its own. Send `t` — **confirm `slot0` appears on host A with no re-pairing prompt**.
5. Send `1`. Send `t` — **confirm `slot1` appears on host B with no re-pairing prompt**.

Steps 4 and 5 are the whole gate. A re-pairing prompt in either direction is a failure.

- [ ] **Step 5: Measure the heap across ten switches**

Alternate `0` and `1` ten times, reading the `heap before/after switch` lines. Record the net delta per cycle.

Expected: roughly flat. A consistent per-cycle loss means the rebuild path leaks — the board will eventually die. If it leaks, note the per-cycle figure; Task 5 will need explicit cleanup or a vendored library, and that is a plan revision, not something to paper over.

- [ ] **Step 6: Confirm bonds survive a reboot**

Press RESET. Send `h`.

Expected: `bonds=2`. If bonds are gone, `deinit(true)` is destroying them — switch to `deinit(false)` and re-run Steps 4–6.

- [ ] **Step 7: Record the findings**

Write the answers into the spec's spike section, replacing the open questions with what actually happened: the working `ble_hs_id_set_rnd` ordering, the heap delta per cycle, and the `deinit` flag that preserves bonds.

```bash
git add docs/superpowers/specs/2026-07-19-multi-device-bonding-design.md
git commit -m "spec: record spike findings for slot switching"
```

**DECISION GATE.** If Step 4 showed re-pairing prompts, stop. Do not continue to Task 2. Fall back to the whitelist architecture described in the spec, record why in `STATUS.md`, and revise this plan. If the spike passed, continue.

- [ ] **Step 8: Delete the spike and return to main**

```bash
rm -rf spike/
git checkout platformio.ini    # drops the build_src_dir override
git add -A && git commit -m "chore: remove slot-switch spike"
git checkout main
git merge --no-ff spike/slot-switch -m "Merge spike findings"
```

---

## Task 2: Native test environment and slot address derivation

**Files:**
- Create: `src/slot_address.h`
- Create: `test/test_slot_address/test_slot_address.cpp`
- Modify: `platformio.ini` (add `[env:native]`, add `CONFIG_BT_NIMBLE_MAX_BONDS=4`)

**Interfaces:**
- Consumes: nothing.
- Produces: `constexpr uint8_t kSlotCount = 4;` and `void deriveSlotAddress(const uint8_t base[6], uint8_t slot, uint8_t out[6]);` — header-only and free of Arduino/BLE includes, so it compiles natively. Task 5 calls it.

Header-only on purpose: with `test_build_src = no`, the native environment does not compile `src/`, so a `.cpp` would force the test to `#include` a source file. Keeping it inline avoids that.

- [ ] **Step 1: Write the failing test**

```cpp
// test/test_slot_address/test_slot_address.cpp
#include <unity.h>

#include "../../src/slot_address.h"

// A base MAC with known low bits, so masking is visible in the assertions.
static const uint8_t kBase[6] = {0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33};

void test_top_two_bits_set_for_static_random(void) {
  uint8_t out[6];
  deriveSlotAddress(kBase, 0, out);
  // BLE requires the top two bits of the most significant byte set.
  TEST_ASSERT_EQUAL_HEX8(0xC0, out[0] & 0xC0);
}

void test_slot_index_lands_in_lsb(void) {
  uint8_t out[6];
  deriveSlotAddress(kBase, 2, out);
  TEST_ASSERT_EQUAL_HEX8(0x32, out[5]);  // 0x33 & 0xFC | 2
}

void test_every_slot_differs(void) {
  uint8_t seen[kSlotCount][6];
  for (uint8_t s = 0; s < kSlotCount; s++) deriveSlotAddress(kBase, s, seen[s]);
  for (uint8_t a = 0; a < kSlotCount; a++) {
    for (uint8_t b = a + 1; b < kSlotCount; b++) {
      TEST_ASSERT_NOT_EQUAL(0, memcmp(seen[a], seen[b], 6));
    }
  }
}

void test_derivation_is_stable(void) {
  uint8_t first[6], second[6];
  deriveSlotAddress(kBase, 1, first);
  deriveSlotAddress(kBase, 1, second);
  TEST_ASSERT_EQUAL_MEMORY(first, second, 6);
}

void test_middle_bytes_come_from_the_base_mac(void) {
  uint8_t out[6];
  deriveSlotAddress(kBase, 0, out);
  TEST_ASSERT_EQUAL_MEMORY(kBase + 1, out + 1, 4);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_top_two_bits_set_for_static_random);
  RUN_TEST(test_slot_index_lands_in_lsb);
  RUN_TEST(test_every_slot_differs);
  RUN_TEST(test_derivation_is_stable);
  RUN_TEST(test_middle_bytes_come_from_the_base_mac);
  return UNITY_END();
}
```

- [ ] **Step 2: Add the native environment**

Append to `platformio.ini`:

```ini
; Host-side unit tests for the pure logic — address derivation and the line
; parser — neither of which needs a board. `test_build_src = no` keeps src/ out
; of the native build, since everything else in there pulls in Arduino.
[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17
test_build_src = no
```

Also add to the existing `[env:esp32-s3-devkitc-1]` `build_flags`, keeping the comment:

```ini
  ; Four bond slots, not NimBLE's default 3 — one per device the proxy can hold
  ; a bond with. Must match kSlotCount in src/slot_address.h.
  -DCONFIG_BT_NIMBLE_MAX_BONDS=4
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
pio test -e native
```
Expected: FAIL — compilation error, `src/slot_address.h` does not exist.

- [ ] **Step 4: Write the implementation**

```cpp
// src/slot_address.h
#pragma once

#include <stdint.h>
#include <string.h>

// Per-slot BLE identity addresses.
//
// Each slot advertises under its own static-random address, so a host only ever
// sees the identity it bonded to and cannot reconnect while another slot is
// active. That isolation is the whole point: sharing one address across slots
// means relying on whitelist filtering to suppress the wrong host, which is
// unreliable on this controller.
//
// Header-only and free of Arduino/BLE includes so it can be unit-tested natively.

constexpr uint8_t kSlotCount = 4;  // must match CONFIG_BT_NIMBLE_MAX_BONDS

// Derives slot `slot`'s address from the chip's base MAC. Both arrays are six
// bytes, most-significant byte first. Deterministic, so the same board always
// produces the same addresses and bonds survive a reflash.
inline void deriveSlotAddress(const uint8_t base[6], uint8_t slot, uint8_t out[6]) {
  memcpy(out, base, 6);
  out[0] |= 0xC0;  // BLE requires both top bits set for a static random address
  out[5] = (uint8_t)((base[5] & 0xFC) | (slot & 0x03));
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
pio test -e native
```
Expected: PASS, 5 tests.

- [ ] **Step 6: Confirm the firmware build still works**

```bash
pio run
```
Expected: success, roughly `RAM 9.1% / Flash 7.6%`. A flash figure near 885KB means `-DUSE_NIMBLE` was lost.

- [ ] **Step 7: Commit**

```bash
git add platformio.ini src/slot_address.h test/test_slot_address/test_slot_address.cpp
git commit -m "Add per-slot BLE address derivation and a native test env"
```

---

## Task 3: Slot verbs in the wire protocol

**Files:**
- Modify: `src/hid_report.h`
- Modify: `src/serial_proto.cpp:70-136`
- Modify: `src/serial_proto.h` (doc comment listing the verbs)
- Create: `test/test_serial_proto/test_serial_proto.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `CommandKind::SelectSlot`, `CommandKind::PairSlot`, `CommandKind::ForgetSlot`, and `Command::slot` (a `uint8_t`). Task 5 routes on these.

This layer stays BLE-ignorant. It parses and range-checks; it has no idea what a slot means.

- [ ] **Step 1: Write the failing test**

```cpp
// test/test_serial_proto/test_serial_proto.cpp
#include <unity.h>

// test_build_src = no keeps src/ out of the native build, so the unit under
// test is included directly. serial_proto has no Arduino dependencies.
#include "../../src/serial_proto.cpp"

// Feeds a whole line through the parser and returns the resulting command.
static Command parse(const char *line) {
  SerialProto proto;
  Command last;
  for (const char *p = line; *p; p++) last = proto.feed(*p);
  return proto.feed('\n');
}

void test_select_slot(void) {
  const Command c = parse("S 2");
  TEST_ASSERT_TRUE(c.kind == CommandKind::SelectSlot);
  TEST_ASSERT_EQUAL_UINT8(2, c.slot);
}

void test_pair_slot(void) {
  const Command c = parse("B 0");
  TEST_ASSERT_TRUE(c.kind == CommandKind::PairSlot);
  TEST_ASSERT_EQUAL_UINT8(0, c.slot);
}

void test_forget_slot(void) {
  const Command c = parse("U 3");
  TEST_ASSERT_TRUE(c.kind == CommandKind::ForgetSlot);
  TEST_ASSERT_EQUAL_UINT8(3, c.slot);
}

void test_slot_out_of_range_is_an_error(void) {
  TEST_ASSERT_TRUE(parse("S 4").kind == CommandKind::Error);
  TEST_ASSERT_TRUE(parse("S 9").kind == CommandKind::Error);
}

void test_missing_slot_is_an_error(void) {
  TEST_ASSERT_TRUE(parse("S").kind == CommandKind::Error);
}

void test_trailing_junk_is_an_error(void) {
  TEST_ASSERT_TRUE(parse("S 1 1").kind == CommandKind::Error);
}

void test_lowercase_verb_works(void) {
  // The existing verbs are case-insensitive; these must not be an exception.
  TEST_ASSERT_TRUE(parse("s 1").kind == CommandKind::SelectSlot);
}

void test_existing_verbs_still_parse(void) {
  const Command k = parse("K 02 04 00 00 00 00 00");
  TEST_ASSERT_TRUE(k.kind == CommandKind::Key);
  TEST_ASSERT_EQUAL_UINT8(0x02, k.key.modifiers);
  TEST_ASSERT_EQUAL_UINT8(0x04, k.key.keys[0]);
  TEST_ASSERT_TRUE(parse("P").kind == CommandKind::Ping);
  TEST_ASSERT_TRUE(parse("R").kind == CommandKind::ReleaseAll);
  TEST_ASSERT_TRUE(parse("C 08 00").kind == CommandKind::Consumer);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_select_slot);
  RUN_TEST(test_pair_slot);
  RUN_TEST(test_forget_slot);
  RUN_TEST(test_slot_out_of_range_is_an_error);
  RUN_TEST(test_missing_slot_is_an_error);
  RUN_TEST(test_trailing_junk_is_an_error);
  RUN_TEST(test_lowercase_verb_works);
  RUN_TEST(test_existing_verbs_still_parse);
  return UNITY_END();
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
pio test -e native -f test_serial_proto
```
Expected: FAIL — `CommandKind::SelectSlot` is not a member.

- [ ] **Step 3: Extend the command seam**

In `src/hid_report.h`, extend the enum and struct:

```cpp
// What the parser produces.
enum class CommandKind {
  None,      // nothing complete yet, or a blank line
  Key,       // full keyboard state in `key`
  Consumer,  // media bitmask in `consumer`
  ReleaseAll,
  Ping,
  SelectSlot,  // make `slot` the active bond slot
  PairSlot,    // clear `slot`'s bond and advertise openly to pair
  ForgetSlot,  // drop `slot`'s bond
  Error,       // malformed line; `error` holds a short reason
};

struct Command {
  CommandKind kind = CommandKind::None;
  HidReport key;
  ConsumerReport consumer;
  uint8_t slot = 0;  // for SelectSlot / PairSlot / ForgetSlot
  const char *error = nullptr;
};
```

- [ ] **Step 4: Parse the verbs**

In `src/serial_proto.cpp`, add this helper inside the anonymous namespace, after `atEnd`:

```cpp
// Parses the single slot argument shared by S, B and U. Slots are a small
// decimal index, not a hex byte — they are not a wire value like a usage code,
// and "S 10" should be rejected rather than read as sixteen.
bool parseSlot(const char *s, size_t len, size_t &pos, uint8_t &out) {
  while (pos < len && isspace((unsigned char)s[pos])) pos++;
  if (pos >= len) return false;
  const char c = s[pos];
  if (c < '0' || c > '9') return false;
  pos++;
  const uint8_t slot = (uint8_t)(c - '0');
  if (slot >= kSlotCount) return false;
  if (pos < len && !isspace((unsigned char)s[pos])) return false;
  out = slot;
  return true;
}
```

Add `#include "slot_address.h"` at the top of `src/serial_proto.cpp` for `kSlotCount`. This does not violate the seam — `slot_address.h` is a plain constant and derivation function with no BLE dependency.

Then add these cases to the `switch (verb)` block, before `default:`:

```cpp
    case 'S':
    case 'B':
    case 'U': {
      uint8_t slot;
      if (!parseSlot(line_, len_, pos, slot)) {
        cmd.kind = CommandKind::Error;
        cmd.error = "slot must be a single digit 0-3";
        return cmd;
      }
      if (!atEnd(line_, len_, pos)) {
        cmd.kind = CommandKind::Error;
        cmd.error = "slot command takes one argument";
        return cmd;
      }
      cmd.slot = slot;
      cmd.kind = (verb == 'S')   ? CommandKind::SelectSlot
                 : (verb == 'B') ? CommandKind::PairSlot
                                 : CommandKind::ForgetSlot;
      return cmd;
    }
```

Update the `default:` case message:

```cpp
      cmd.error = "unknown command (expected K, C, R, P, S, B or U)";
```

- [ ] **Step 5: Update the protocol comment**

In `src/serial_proto.h`, extend the doc comment listing the wire format to cover the three new verbs and their replies, matching the spec's table:

```
//   S <slot>   select the active bond slot     -> OK slot 2 bonded advertising
//   B <slot>   clear the slot and pair anew    -> OK pairing 2
//   U <slot>   forget the slot's bond          -> OK cleared 2
//
// These reply OK rather than SENT: unlike a HID report, a slot change is a local
// action the firmware genuinely knows it performed. OK still says nothing about
// whether the host reconnected — only the device can show that.
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
pio test -e native
```
Expected: PASS, 13 tests across both suites.

- [ ] **Step 7: Confirm the firmware still builds**

```bash
pio run
```
Expected: success. `main.cpp` does not yet handle the new kinds — that is Task 5 — but its `switch` has no `default`, so new enum values may produce a `-Wswitch` warning. That is expected here and goes away in Task 5.

- [ ] **Step 8: Commit**

```bash
git add src/hid_report.h src/serial_proto.cpp src/serial_proto.h test/test_serial_proto/test_serial_proto.cpp
git commit -m "Parse S/B/U slot verbs in the line protocol"
```

---

## Task 4: Slot persistence

**Files:**
- Create: `src/slot_store.h`
- Create: `src/slot_store.cpp`

**Interfaces:**
- Consumes: `kSlotCount` from `src/slot_address.h`.
- Produces: `class SlotStore` with `void begin()`, `uint8_t activeSlot() const`, `void setActiveSlot(uint8_t)`, `bool peerFor(uint8_t slot, uint8_t out[6]) const`, `void setPeer(uint8_t slot, const uint8_t addr[6])`, `void clearPeer(uint8_t slot)`, `bool isBonded(uint8_t slot) const`. Task 5 owns an instance.

This exists because NimBLE's bond store is keyed by peer identity address and keeps no record of which local identity a bond was created against — `getBondedAddress()` wraps `ble_store_util_bonded_peers()`, a flat list in store order. Slot number is therefore unrecoverable from it. Without this map, `U <slot>` has no bond to delete and the `bonded`/`empty` status field cannot be computed.

No native test: this is a thin wrapper over Arduino `Preferences` with no logic worth mocking. Its behaviour is verified on hardware in Task 5.

- [ ] **Step 1: Write the header**

```cpp
// src/slot_store.h
#pragma once

#include <Preferences.h>
#include <stdint.h>

#include "slot_address.h"

// Persists which slot is active and which peer bonded to each slot.
//
// The peer map is not a redundant copy of NimBLE's bond store. NimBLE keys bonds
// by the peer's identity address and records nothing about which local identity
// the bond was made against, so there is no way to ask it "which peer is in slot
// 2". This is the only record of that association, and without it a slot cannot
// be forgotten or reported as bonded.
//
// Knows nothing about serial, and nothing about BLE beyond the shape of an address.

class SlotStore {
 public:
  void begin();

  uint8_t activeSlot() const { return active_; }
  void setActiveSlot(uint8_t slot);

  // Writes slot's peer identity address into `out` (6 bytes, MSB first) and its
  // NimBLE address type into `outType`. Returns false when the slot has no bond,
  // leaving both untouched.
  //
  // The type is stored, not assumed. iOS identity addresses are random-static,
  // but some Android devices use a public identity address, and reconstructing
  // one with the wrong type means deleteBond() silently matches nothing — the
  // slot would report itself forgotten while the bond survived.
  bool peerFor(uint8_t slot, uint8_t out[6], uint8_t &outType) const;

  // Returns true when this differs from what was already stored, so the caller
  // can skip a redundant NVS write on every reconnect.
  bool setPeer(uint8_t slot, const uint8_t addr[6], uint8_t type);
  void clearPeer(uint8_t slot);

  bool isBonded(uint8_t slot) const;

 private:
  Preferences prefs_;
  uint8_t active_ = 0;
  uint8_t peers_[kSlotCount][6] = {};
  uint8_t peerTypes_[kSlotCount] = {};
  bool bonded_[kSlotCount] = {};
};
```

- [ ] **Step 2: Write the implementation**

```cpp
// src/slot_store.cpp
#include "slot_store.h"

#include <string.h>

namespace {

constexpr char kNamespace[] = "bleslots";
constexpr char kActiveKey[] = "active";

// "peer0".."peer3". Caller supplies the buffer; Preferences keys are short-lived.
void peerKey(uint8_t slot, char out[8]) { snprintf(out, 8, "peer%u", slot); }

// The address type rides along in a seventh byte rather than a second key —
// one blob, one write, and the two can never disagree.
constexpr size_t kPeerBlobSize = 7;

}  // namespace

void SlotStore::begin() {
  prefs_.begin(kNamespace, false);

  active_ = prefs_.getUChar(kActiveKey, 0);
  if (active_ >= kSlotCount) active_ = 0;  // guard against a corrupt or stale value

  for (uint8_t s = 0; s < kSlotCount; s++) {
    char key[8];
    peerKey(s, key);

    uint8_t blob[kPeerBlobSize];
    if (prefs_.getBytes(key, blob, kPeerBlobSize) == kPeerBlobSize) {
      memcpy(peers_[s], blob, 6);
      peerTypes_[s] = blob[6];
      bonded_[s] = true;
    }
  }
}

void SlotStore::setActiveSlot(uint8_t slot) {
  if (slot >= kSlotCount) return;
  active_ = slot;
  prefs_.putUChar(kActiveKey, slot);
}

bool SlotStore::peerFor(uint8_t slot, uint8_t out[6], uint8_t &outType) const {
  if (slot >= kSlotCount || !bonded_[slot]) return false;
  memcpy(out, peers_[slot], 6);
  outType = peerTypes_[slot];
  return true;
}

bool SlotStore::setPeer(uint8_t slot, const uint8_t addr[6], uint8_t type) {
  if (slot >= kSlotCount) return false;

  // Authentication completes on every reconnect, not just the first bond.
  // Rewriting an unchanged value would burn flash for nothing.
  if (bonded_[slot] && peerTypes_[slot] == type && memcmp(peers_[slot], addr, 6) == 0) {
    return false;
  }

  memcpy(peers_[slot], addr, 6);
  peerTypes_[slot] = type;
  bonded_[slot] = true;

  uint8_t blob[kPeerBlobSize];
  memcpy(blob, addr, 6);
  blob[6] = type;

  char key[8];
  peerKey(slot, key);
  prefs_.putBytes(key, blob, kPeerBlobSize);
  return true;
}

void SlotStore::clearPeer(uint8_t slot) {
  if (slot >= kSlotCount) return;
  memset(peers_[slot], 0, 6);
  peerTypes_[slot] = 0;
  bonded_[slot] = false;

  char key[8];
  peerKey(slot, key);
  prefs_.remove(key);
}

bool SlotStore::isBonded(uint8_t slot) const {
  return slot < kSlotCount && bonded_[slot];
}
```

- [ ] **Step 3: Verify it compiles**

```bash
pio run
```
Expected: success. Nothing references `SlotStore` yet, so this only proves it builds.

- [ ] **Step 4: Commit**

```bash
git add src/slot_store.h src/slot_store.cpp
git commit -m "Persist active slot and the slot -> peer address map"
```

---

## Task 5: Slot switching in the sink, and routing

**Files:**
- Modify: `src/ble_hid_sink.h`
- Modify: `src/ble_hid_sink.cpp`
- Modify: `src/main.cpp:22-58` (routing), `src/main.cpp:79-90` (loop)

**Interfaces:**
- Consumes: `deriveSlotAddress`/`kSlotCount` (Task 2), `CommandKind::SelectSlot`/`PairSlot`/`ForgetSlot` and `Command::slot` (Task 3), `SlotStore` (Task 4).
- Produces: on `BleHidSink` — `bool selectSlot(uint8_t)`, `bool pairSlot(uint8_t)`, `bool forgetSlot(uint8_t)`, `uint8_t activeSlot() const`, `bool isBonded(uint8_t) const`.

**Apply the spike's findings here.** Task 1 established the working `ble_hs_id_set_rnd` ordering, the `deinit` flag that preserves bonds, and whether the rebuild leaks. If the spike found a leak, address it in this task rather than proceeding.

`BleKeyboard kb_` becomes a pointer, because switching identity requires destroying and rebuilding it. It also becomes a subclass: `BleKeyboard` installs itself as the server callbacks and never overrides `onAuthenticationComplete`, so a subclass can capture the bonded peer's identity address without displacing the library's own `onConnect`/`onDisconnect` handling.

- [ ] **Step 1: Rewrite the header**

```cpp
// src/ble_hid_sink.h
#pragma once

#include <BleKeyboard.h>

#include "hid_report.h"
#include "slot_address.h"
#include "slot_store.h"

// Wraps BleKeyboard and presents the HidReport seam. Knows nothing about serial.
//
// Uses sendReport(KeyReport*) directly rather than press()/write(): those run an
// ASCII -> usage-code translation table, which is exactly the layer being bypassed
// for full HID passthrough.
//
// Holds up to kSlotCount bonds, one per device, and advertises under a different
// identity address per slot so only the selected host can reconnect. Switching
// slots restarts the whole BLE stack, which is why the keyboard is a pointer.

// Subclassed only to observe authentication. BleKeyboard registers itself as the
// server callbacks and does not override onAuthenticationComplete, so overriding
// it here captures the bonded peer without disturbing the library's own
// connect/disconnect handling.
class SlotKeyboard : public BleKeyboard {
 public:
  SlotKeyboard(const char *name, const char *manufacturer, SlotStore *store, uint8_t slot)
      : BleKeyboard(name, manufacturer, 100), store_(store), slot_(slot) {}

  void onAuthenticationComplete(ble_gap_conn_desc *desc) override;

 private:
  SlotStore *store_;
  uint8_t slot_;
};

class BleHidSink {
 public:
  BleHidSink(const char *deviceName, const char *manufacturer);

  void begin();

  bool isConnected();

  // Send the full keyboard state. No-ops when nothing is connected, since the
  // report would be dropped anyway.
  void sendKey(const HidReport &report);

  void sendConsumer(const ConsumerReport &report);

  void releaseAll();

  // Make `slot` active: disconnect, restart the stack under that slot's identity
  // address, and advertise. The bonded host reconnects on its own. Returns false
  // only for an out-of-range slot — a true return means the local switch happened,
  // not that anything reconnected.
  bool selectSlot(uint8_t slot);

  // Forget `slot`'s bond, select it, and advertise openly so a new device can pair.
  bool pairSlot(uint8_t slot);

  // Drop `slot`'s bond. Restarts advertising if it was the active slot.
  bool forgetSlot(uint8_t slot);

  uint8_t activeSlot() const { return store_.activeSlot(); }
  bool isBonded(uint8_t slot) const { return store_.isBonded(slot); }

  // Call from loop(). Returns true when the connection state changed since the
  // last call, so the caller can log the edge.
  bool poll();

 private:
  // Brings the stack up under `slot`'s identity. Tears down any existing stack first.
  void startSlot(uint8_t slot);
  void stopStack();

  // Deletes the slot's NimBLE bond and clears it from the store.
  void dropBond(uint8_t slot);

  const char *deviceName_;
  const char *manufacturer_;
  SlotKeyboard *kb_ = nullptr;
  SlotStore store_;
  bool wasConnected_ = false;
};
```

- [ ] **Step 2: Rewrite the implementation**

```cpp
// src/ble_hid_sink.cpp
#include "ble_hid_sink.h"

#include <NimBLEDevice.h>
#include <esp_mac.h>
#include <host/ble_hs.h>

void SlotKeyboard::onAuthenticationComplete(ble_gap_conn_desc *desc) {
  // NimBLE hands us the peer's identity address little-endian; the store keeps
  // addresses MSB-first, matching how they are printed and derived.
  uint8_t addr[6];
  for (int i = 0; i < 6; i++) addr[i] = desc->peer_id_addr.val[5 - i];

  // The type comes from the peer, never assumed: iOS uses random-static identity
  // addresses but some Android devices use public ones, and forgetting a bond
  // with the wrong type matches nothing and fails silently.
  store_->setPeer(slot_, addr, desc->peer_id_addr.type);
}

BleHidSink::BleHidSink(const char *deviceName, const char *manufacturer)
    : deviceName_(deviceName), manufacturer_(manufacturer) {}

void BleHidSink::begin() {
  store_.begin();
  startSlot(store_.activeSlot());
}

void BleHidSink::startSlot(uint8_t slot) {
  uint8_t base[6];
  esp_read_mac(base, ESP_MAC_BT);

  uint8_t addr[6];
  deriveSlotAddress(base, slot, addr);

  NimBLEDevice::init(deviceName_);

  // Little-endian on the wire, MSB-first in our arrays.
  uint8_t rnd[6];
  for (int i = 0; i < 6; i++) rnd[i] = addr[5 - i];
  ble_hs_id_set_rnd(rnd);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);

  kb_ = new SlotKeyboard(deviceName_, manufacturer_, &store_, slot);
  kb_->begin();
}

void BleHidSink::stopStack() {
  NimBLEServer *server = NimBLEDevice::getServer();
  if (server != nullptr) {
    // Disconnect cleanly rather than pulling the stack out from under a live link.
    for (uint16_t handle : server->getPeerDevices()) server->disconnect(handle);
    delay(200);
  }

  delete kb_;
  kb_ = nullptr;

  NimBLEDevice::deinit(true);
  delay(200);

  wasConnected_ = false;
}

void BleHidSink::dropBond(uint8_t slot) {
  uint8_t peer[6];
  uint8_t type;
  if (store_.peerFor(slot, peer, type)) {
    uint8_t rev[6];
    for (int i = 0; i < 6; i++) rev[i] = peer[5 - i];
    NimBLEDevice::deleteBond(NimBLEAddress(rev, type));
  }
  store_.clearPeer(slot);
}

bool BleHidSink::selectSlot(uint8_t slot) {
  if (slot >= kSlotCount) return false;
  stopStack();
  store_.setActiveSlot(slot);
  startSlot(slot);
  return true;
}

bool BleHidSink::pairSlot(uint8_t slot) {
  if (slot >= kSlotCount) return false;

  // Drop the old bond first so the slot advertises openly rather than waiting
  // for a device that may never come back.
  dropBond(slot);

  return selectSlot(slot);
}

bool BleHidSink::forgetSlot(uint8_t slot) {
  if (slot >= kSlotCount) return false;

  dropBond(slot);

  // Restarting only matters when the forgotten slot is the one on air.
  if (slot == store_.activeSlot()) return selectSlot(slot);
  return true;
}

bool BleHidSink::isConnected() { return kb_ != nullptr && kb_->isConnected(); }

void BleHidSink::sendKey(const HidReport &report) {
  if (!isConnected()) return;

  KeyReport hid;
  hid.modifiers = report.modifiers;
  hid.reserved = 0;
  memcpy(hid.keys, report.keys, sizeof(hid.keys));

  kb_->sendReport(&hid);
}

void BleHidSink::sendConsumer(const ConsumerReport &report) {
  if (!isConnected()) return;

  MediaKeyReport media;
  media[0] = report.bytes[0];
  media[1] = report.bytes[1];

  kb_->sendReport(&media);
}

void BleHidSink::releaseAll() {
  if (isConnected()) kb_->releaseAll();
}

bool BleHidSink::poll() {
  // No held-key state is tracked here on purpose. Reports are full state, so the
  // producer always sends the complete current picture and a stale shadow copy
  // would only be a second source of truth to keep in sync. A host that drops
  // mid-chord simply sees no further reports; on reconnect the next report
  // re-establishes the full state.
  const bool connected = isConnected();
  const bool changed = (connected != wasConnected_);
  wasConnected_ = connected;
  return changed;
}
```

- [ ] **Step 3: Route the new commands**

In `src/main.cpp`, add these cases to `handle()` before `case CommandKind::Error:`:

```cpp
    // "OK" here, not "SENT". Unlike a HID report, a slot change is a local action
    // the firmware genuinely performed and can confirm. It still says nothing
    // about whether the host reconnected — only the device shows that.
    case CommandKind::SelectSlot:
      if (sink.selectSlot(cmd.slot)) {
        Serial.printf("OK slot %u %s %s\n", cmd.slot,
                      sink.isBonded(cmd.slot) ? "bonded" : "empty",
                      sink.isConnected() ? "connected" : "advertising");
      } else {
        Serial.println("ERR bad slot");
      }
      break;

    case CommandKind::PairSlot:
      Serial.println(sink.pairSlot(cmd.slot) ? "OK pairing" : "ERR bad slot");
      break;

    case CommandKind::ForgetSlot:
      Serial.println(sink.forgetSlot(cmd.slot) ? "OK cleared" : "ERR bad slot");
      break;
```

Extend the `Ping` case to report the active slot:

```cpp
    case CommandKind::Ping:
      Serial.printf("OK %s slot %u %s\n", sink.isConnected() ? "connected" : "advertising",
                    sink.activeSlot(), sink.isBonded(sink.activeSlot()) ? "bonded" : "empty");
      break;
```

Update the banner in `setup()`:

```cpp
  Serial.println("commands: K <mod> <k1..k6> | C <b0> <b1> | R | P | S <n> | B <n> | U <n>");
```

- [ ] **Step 4: Build and flash**

```bash
pio run && pio run -t upload
```
Expected: build succeeds, flash figure still well under 885KB.

- [ ] **Step 5: Verify on hardware with two devices**

Close the monitor before running host commands. With `pio device monitor` open, typing commands by hand:

1. `B 0`, pair host A, then `P` — expect `OK connected slot 0 bonded`.
2. `B 1`, pair host B, then `P` — expect `OK connected slot 1 bonded`.
3. `S 0` — expect `OK slot 0 bonded ...`; wait for host A to reconnect, then `K 02 04 00 00 00 00 00` followed by `K 00 00 00 00 00 00 00`. **Confirm a capital A appears on host A.**
4. `S 1`, wait, same two `K` lines. **Confirm a capital A appears on host B.**
5. Press RESET, then `P`. Expect the active slot to be 1, not 0 — the NVS write survived.
6. `U 1` then `P` — expect slot 1 reported `empty`.
7. `S 0` and type again. **Confirm host A still works and was not disturbed by forgetting slot 1.**

Steps 3, 4 and 7 must be confirmed by a human watching the devices. A `SENT` reply is not evidence.

- [ ] **Step 6: Commit**

```bash
git add src/ble_hid_sink.h src/ble_hid_sink.cpp src/main.cpp
git commit -m "Switch BLE identity per slot, with bonds persisted across reboots"
```

---

## Task 6: Host-side slot config and link verbs

**Files:**
- Modify: `host/proxy_term.py` (add `SLOT_COUNT`, `DEFAULT_SLOT_TARGETS`, `load_slot_config`, `save_slot_config`, `slot_config_path`; add three `Link` methods near `release_all` at `host/proxy_term.py:281`)
- Modify: `host/test_proxy_term.py` (add cases)

**Interfaces:**
- Consumes: the `S`/`B`/`U` wire verbs from Task 3.
- Produces: `load_slot_config(path)` → `dict[int, str]` mapping slot number to a `TARGETS` key; `save_slot_config(path, mapping)`; `Link.select_slot(n)`, `Link.pair_slot(n)`, `Link.forget_slot(n)`, each returning the firmware's reply string.

The config is pure I/O over a small JSON file, so the tests cover it without hardware — which is the point, since a wrong slot→target mapping would silently send iPad chords to an Android phone and look like a firmware bug.

- [ ] **Step 1: Write the failing tests**

Append to `host/test_proxy_term.py`, before the runner at the bottom:

```python
@case("slot config round-trips through JSON")
def test_slot_config_roundtrip():
    import tempfile, os
    mapping = {0: "iphone", 1: "ipad", 2: "android", 3: "android"}
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "slots.json")
        pt.save_slot_config(path, mapping)
        assert pt.load_slot_config(path) == mapping


@case("a missing slot config falls back to defaults rather than failing")
def test_slot_config_missing():
    # Startup must survive a fresh checkout with no config written yet.
    assert pt.load_slot_config("/nonexistent/slots.json") == pt.DEFAULT_SLOT_TARGETS


@case("a corrupt slot config falls back to defaults")
def test_slot_config_corrupt():
    import tempfile, os
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "slots.json")
        with open(path, "w") as f:
            f.write("{not json")
        assert pt.load_slot_config(path) == pt.DEFAULT_SLOT_TARGETS


@case("slot config rejects unknown targets and out-of-range slots")
def test_slot_config_validates():
    import tempfile, os, json
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "slots.json")
        with open(path, "w") as f:
            json.dump({"0": "nokia", "9": "ipad", "1": "iphone"}, f)
        loaded = pt.load_slot_config(path)
        # A bad target and a bad slot are dropped to their defaults; the good
        # entry survives. Silently honouring "nokia" would KeyError later, deep
        # inside a keystroke.
        assert loaded[0] == pt.DEFAULT_SLOT_TARGETS[0]
        assert loaded[1] == "iphone"
        assert 9 not in loaded
        assert set(loaded) == set(range(pt.SLOT_COUNT))


@case("every default slot target names a real TARGETS entry")
def test_default_slot_targets_are_valid():
    assert set(pt.DEFAULT_SLOT_TARGETS) == set(range(pt.SLOT_COUNT))
    for target in pt.DEFAULT_SLOT_TARGETS.values():
        assert target in pt.TARGETS
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
~/.platformio/penv/bin/python host/test_proxy_term.py slot
```
Expected: FAIL — `module 'proxy_term' has no attribute 'save_slot_config'`.

- [ ] **Step 3: Implement the config layer**

Add to `host/proxy_term.py`, after the `COMMANDS` dict (around line 189):

```python
SLOT_COUNT = 4

# Which keymap each slot drives. The chords for Home and the app switcher
# genuinely differ per OS, so the keymap has to follow the device — switching to
# the iPad slot and still sending iPhone chords would look like a firmware bug.
DEFAULT_SLOT_TARGETS = {0: "iphone", 1: "ipad", 2: "android", 3: "android"}


def slot_config_path():
    """Where the slot -> target mapping lives, beside this script."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "slots.json")


def load_slot_config(path):
    """Read the slot -> target mapping, falling back to defaults entry by entry.

    Anything unreadable, malformed, or naming a target that no longer exists
    falls back rather than raising. A bad entry that survived to keystroke time
    would KeyError deep inside the input loop, far from its cause.
    """
    mapping = dict(DEFAULT_SLOT_TARGETS)
    try:
        with open(path) as f:
            raw = json.load(f)
    except (OSError, ValueError):
        return mapping

    if not isinstance(raw, dict):
        return mapping

    for key, target in raw.items():
        try:
            slot = int(key)
        except (TypeError, ValueError):
            continue
        if 0 <= slot < SLOT_COUNT and target in TARGETS:
            mapping[slot] = target
    return mapping


def save_slot_config(path, mapping):
    """Persist the slot -> target mapping. JSON object keys are always strings."""
    with open(path, "w") as f:
        json.dump({str(slot): target for slot, target in sorted(mapping.items())}, f, indent=2)
        f.write("\n")
```

Add `import json` and `import os` to the imports at the top of the file if they are not already present.

- [ ] **Step 4: Add the Link methods**

In `host/proxy_term.py`, add to `class Link` immediately after `release_all` (around line 283):

```python
    def select_slot(self, slot):
        """Make `slot` active. Returns the firmware's reply.

        The reply says the switch happened locally, not that the device
        reconnected — that takes a second or two and is only observable there.
        """
        return self._cmd(f"S {slot}")

    def pair_slot(self, slot):
        """Clear `slot`'s bond and advertise openly so a new device can pair."""
        return self._cmd(f"B {slot}")

    def forget_slot(self, slot):
        """Drop `slot`'s bond."""
        return self._cmd(f"U {slot}")
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
~/.platformio/penv/bin/python host/test_proxy_term.py
```
Expected: PASS, 17/17 (the existing 12 plus 5 new).

- [ ] **Step 6: Commit**

```bash
git add host/proxy_term.py host/test_proxy_term.py
git commit -m "Host: slot -> target config and the S/B/U link verbs"
```

---

## Task 7: Host-side leader bindings

**Files:**
- Modify: `host/proxy_term.py` — `command_help` at `host/proxy_term.py:209`, the module docstring at `host/proxy_term.py:11`, and the leader dispatch in `main()`
- Modify: `host/test_proxy_term.py` (help-text case)

**Interfaces:**
- Consumes: `load_slot_config`/`save_slot_config`/`SLOT_COUNT` and the `Link` slot methods from Task 6.
- Produces: no new API — this is the interactive wiring.

Bindings, extending the existing `Ctrl-]` leader:

- `Ctrl-]` then `1`–`4` — select that slot and swap the active keymap with it.
- `Ctrl-]` then `!` then `1`–`4` — pair that slot.
- `Ctrl-]` then `?` — now also lists slots, their targets, and which is active.

Digits are `1`–`4` for slots 0–3: a keyboard has no `0` next to `1` in muscle-memory terms, and one-based labels match how the slots get talked about.

- [ ] **Step 1: Write the failing test**

Append to `host/test_proxy_term.py`:

```python
@case("help lists slot bindings and marks the active slot")
def test_help_lists_slots():
    text = pt.command_help("iphone", slots={0: "iphone", 1: "ipad", 2: "android",
                                            3: "android"}, active=1)
    assert "Ctrl-] 2" in text          # slot 1 is bound to leader key "2"
    assert "ipad" in text
    assert "active" in text
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
~/.platformio/penv/bin/python host/test_proxy_term.py "help lists slot"
```
Expected: FAIL — `command_help() got an unexpected keyword argument 'slots'`.

- [ ] **Step 3: Extend command_help**

Replace `command_help` in `host/proxy_term.py`:

```python
def command_help(target, slots=None, active=None):
    """One-line-per-command summary of the leader bindings for `target`.

    `slots` maps slot number to target name; `active` is the current slot. Both
    are optional so the function stays usable before a link exists.
    """
    lines = [f"Ctrl-] commands ({target}):"]
    for key, (name, label) in sorted(COMMANDS.items()):
        action = TARGETS[target][name]
        if action[0] == "X":
            note = "unavailable"
        else:
            note = "chord" if action[0] == "K" else "consumer"
        lines.append(f"  Ctrl-] {key}   {label} ({note})")

    if slots is not None:
        lines.append("  devices:")
        for slot in range(SLOT_COUNT):
            mark = " (active)" if slot == active else ""
            lines.append(f"  Ctrl-] {slot + 1}   slot {slot}: {slots[slot]}{mark}")
        lines.append("  Ctrl-] ! N Pair slot N with a new device")

    lines.append("  Ctrl-] q   Quit")
    lines.append("  Ctrl-] ?   This list")
    return "\n".join(lines)
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
~/.platformio/penv/bin/python host/test_proxy_term.py
```
Expected: PASS, 18/18.

- [ ] **Step 5: Wire the leader dispatch**

In `main()`, the leader handler currently looks up `COMMANDS` and quits on `q`. Extend it so that, after the existing `COMMANDS` lookup fails:

```python
                # Digits 1-4 select a bond slot and swap the keymap with it.
                if leader_ch in "1234":
                    slot = int(leader_ch) - 1
                    reply = link.select_slot(slot)
                    target = slots[slot]
                    commands = TARGETS[target]
                    out(f"\r\nslot {slot} ({target}): {reply}\r\n")
                    out("the device takes a moment to reconnect\r\n")
                    continue

                # "!" then a digit pairs that slot with a new device.
                if leader_ch == "!":
                    digit = read1()
                    if digit not in "1234":
                        out("\r\npair: expected a slot digit 1-4\r\n")
                        continue
                    slot = int(digit) - 1
                    reply = link.pair_slot(slot)
                    out(f"\r\nslot {slot} pairing: {reply}\r\n")
                    out(f"pair with \"Proxy Keyboard\" on the device now\r\n")
                    continue
```

Before the input loop, load the config and pick the starting target from the active slot:

```python
    slots = load_slot_config(slot_config_path())

    # --target still wins when given explicitly; otherwise the active slot decides,
    # since the whole point of slots is that the keymap follows the device.
    state = link.ping()
    active = 0
    if " slot " in state:
        try:
            active = int(state.split(" slot ")[1].split()[0])
        except (IndexError, ValueError):
            active = 0
    if not args.target_given:
        args.target = slots[active]
```

Change the `--target` argument so an explicit value is distinguishable from the default:

```python
    ap.add_argument("--target", default=None, choices=sorted(TARGETS),
                    help="which device the Ctrl-] commands target; overrides the "
                         "keymap the active bond slot would otherwise select")
```

and after parsing:

```python
    args.target_given = args.target is not None
    if args.target is None:
        args.target = "android"  # replaced by the active slot's target below
```

Update `commands = TARGETS[args.target]` to run after that resolution, and pass the slot data into the `?` help call: `command_help(args.target, slots=slots, active=active)`.

- [ ] **Step 6: Update the module docstring**

In the docstring at `host/proxy_term.py:11`, extend the leader-key description to mention slot selection and pairing, matching the new bindings.

- [ ] **Step 7: Verify interactively**

Close the monitor first. With two devices bonded from Task 5:

```bash
~/.platformio/penv/bin/python host/proxy_term.py
```

1. Press `Ctrl-]` then `?` — confirm the slot list appears with the right targets and the active slot marked.
2. Press `Ctrl-]` then `2`, wait for reconnect, and type. **Confirm the text lands on the slot-1 device.**
3. Press `Ctrl-]` then `h`. **Confirm the Home behaviour matches that device's keymap**, not the previous one.
4. Press `Ctrl-]` then `1`, wait, and type. **Confirm text lands on the slot-0 device.**
5. Press `Ctrl-]` then `q` and confirm the terminal is restored.

- [ ] **Step 7a: Smoke-test the non-interactive entry points**

This task rewrote `--target` resolution, which `--text` and `--probe` also pass through. They have no test coverage and would regress silently.

```bash
~/.platformio/penv/bin/python host/proxy_term.py --text 'hello'
~/.platformio/penv/bin/python host/proxy_term.py --probe C:80:00
~/.platformio/penv/bin/python host/proxy_term.py --target iphone --text 'hi'
```

Expected: each exits cleanly. **Confirm on the device** that `hello` was typed, that `C:80:00` triggered Home, and that the explicit `--target` was honoured rather than being overridden by the active slot's mapping.

- [ ] **Step 8: Commit**

```bash
git add host/proxy_term.py host/test_proxy_term.py
git commit -m "Host: leader bindings for slot selection and pairing"
```

---

## Task 8: Documentation

**Files:**
- Modify: `STATUS.md` (wire protocol table, verified behaviour, roadmap)
- Modify: `CLAUDE.md` (commands section — the native test runner)

**Interfaces:**
- Consumes: the verified results of Tasks 5 and 7.
- Produces: nothing code-facing.

- [ ] **Step 1: Extend the wire protocol table in STATUS.md**

Add the three verbs to the table, and a note that they reply `OK` rather than `SENT` because a slot switch is a local action the firmware can actually confirm — while still saying nothing about whether the host reconnected.

- [ ] **Step 2: Add a slot table to the verified-behaviour section**

Same VERIFIED/UNVERIFIED discipline as `TARGETS`. Record only what a human actually watched:

```markdown
### Bond slots

Four slots, one bond each, one active at a time. Each advertises under its own
identity address, so only the selected host can reconnect.

| Slot | Device | Keymap | Status |
|---|---|---|---|
| 0 | | | |
| 1 | | | |
| 2 | — | — | empty |
| 3 | — | — | empty |

Switching back and forth without re-pairing: VERIFIED / UNVERIFIED — fill in
from the Task 5 and Task 7 runs, naming the two devices actually used.
```

- [ ] **Step 3: Update the roadmap**

Move multi-device bonding from **Next** to **Done**, keeping the same shape as the other entries and noting what was verified on which devices. Promote the macOS CGEventTap milestone to **Next**.

- [ ] **Step 4: Add the native test command to both docs**

In `CLAUDE.md`'s commands block and `STATUS.md`'s command block:

```bash
pio test -e native   # C++ unit tests for the pure layer — no hardware needed
```

- [ ] **Step 5: Record the heap finding if the spike found a leak**

If Task 1 Step 5 measured a per-cycle heap loss, add it to the Constraints section of `STATUS.md` with the measured figure and the switch count it implies. A future reader debugging a board that dies after N switches needs this.

- [ ] **Step 6: Run everything one last time**

```bash
pio test -e native
~/.platformio/penv/bin/python host/test_proxy_term.py
pio run
```
Expected: all C++ tests pass, 18/18 host tests pass, firmware builds at roughly `RAM 9.1% / Flash 7.6%`.

- [ ] **Step 7: Commit**

```bash
git add STATUS.md CLAUDE.md
git commit -m "Document bond slots in STATUS.md"
```
