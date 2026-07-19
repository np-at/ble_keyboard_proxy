#pragma once

#include "hid_report.h"

// Line-based ASCII protocol, all values hex. Deliberately typeable by hand in a
// serial monitor so the firmware is testable with no host software:
//
//   K <mod> <k1> <k2> <k3> <k4> <k5> <k6>   full keyboard state (8 bytes)
//   C <b0> <b1>                             consumer/media bitmask
//   R                                       release all
//   P                                       ping -> "OK <state>"
//   S <slot>   select the active bond slot     -> OK slot 2 bonded advertising
//   B <slot>   clear the slot and pair anew    -> OK pairing 2
//   U <slot>   forget the slot's bond          -> OK cleared 2
//
// These reply OK rather than SENT: unlike a HID report, a slot change is a local
// action the firmware genuinely knows it performed. OK still says nothing about
// whether the host reconnected — only the device can show that.
//
// Example, a capital A:
//   K 02 04 00 00 00 00 00
//   K 00 00 00 00 00 00 00
//
// Knows nothing about BLE. Feed it bytes, get Commands out.

class SerialProto {
 public:
  // Feed one incoming byte. Returns a Command with kind != None only when a
  // complete line has been terminated by \n or \r.
  Command feed(char c);

 private:
  static constexpr size_t kMaxLine = 64;

  Command parseLine();

  char line_[kMaxLine];
  size_t len_ = 0;
  bool overflow_ = false;
};
