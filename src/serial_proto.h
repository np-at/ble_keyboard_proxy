#pragma once

#include "hid_report.h"

// Line-based ASCII protocol, all values hex. Deliberately typeable by hand in a
// serial monitor so the firmware is testable with no host software:
//
//   K <mod> <k1> <k2> <k3> <k4> <k5> <k6>   full keyboard state (8 bytes)
//   C <b0> <b1>                             consumer/media bitmask
//   R                                       release all
//   P                                       ping -> "OK <state>"
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
