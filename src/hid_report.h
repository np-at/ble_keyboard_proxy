#pragma once

#include <stdint.h>
#include <string.h>

// The seam between whatever produces keystrokes and whatever consumes them.
//
// HID boot-protocol keyboard reports are state, not events: one report describes
// every key currently held down. Producers therefore emit the complete current
// state on every change, and hold/repeat/chording all fall out for free.
//
// Today the producer is a serial line parser. Later it could be a real keyboard on
// the S3's USB-OTG host port — that swap should not touch the BLE side.

struct HidReport {
  uint8_t modifiers = 0;  // bitmask: LCtrl LShift LAlt LGui RCtrl RShift RAlt RGui
  uint8_t keys[6] = {0};  // HID usage codes, 0 = empty slot

  bool operator==(const HidReport &other) const {
    return modifiers == other.modifiers && memcmp(keys, other.keys, sizeof(keys)) == 0;
  }
  bool operator!=(const HidReport &other) const { return !(*this == other); }
};

// T-vK's consumer report is a bitmask over 16 predefined keys baked into its HID
// descriptor — NOT the 16-bit consumer usage space. See KEY_MEDIA_* in BleKeyboard.h.
// byte0 bit3 (0x08) is play/pause, byte1 bit1 (0x02) is calculator, and so on.
struct ConsumerReport {
  uint8_t bytes[2] = {0, 0};
};

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
