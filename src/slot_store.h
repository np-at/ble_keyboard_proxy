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
