#pragma once

#include <BleKeyboard.h>

#include "hid_report.h"

// Wraps BleKeyboard and presents the HidReport seam. Knows nothing about serial.
//
// Uses sendReport(KeyReport*) directly rather than press()/write(): those run an
// ASCII -> usage-code translation table, which is exactly the layer being bypassed
// for full HID passthrough.

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

  // Call from loop(). Detects a disconnect edge and clears our shadow state so a
  // reconnecting host doesn't inherit keys the previous one left held.
  // Returns true when the connection state changed since the last call, so the
  // caller can log the edge.
  bool poll();

 private:
  BleKeyboard kb_;
  bool wasConnected_ = false;
};
