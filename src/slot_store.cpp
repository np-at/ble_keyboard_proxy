// src/slot_store.cpp
#include "slot_store.h"

#include <stdio.h>
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
