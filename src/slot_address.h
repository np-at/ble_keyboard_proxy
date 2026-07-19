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
