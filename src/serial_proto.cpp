#include "serial_proto.h"

#include <ctype.h>

namespace {

// Returns -1 if not a hex digit.
int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Pulls the next whitespace-separated hex byte out of `s`, advancing `pos`.
// Accepts one or two hex digits so "K 0 4" works as well as "K 00 04".
bool nextHexByte(const char *s, size_t len, size_t &pos, uint8_t &out) {
  while (pos < len && isspace((unsigned char)s[pos])) pos++;
  if (pos >= len) return false;

  int hi = hexVal(s[pos]);
  if (hi < 0) return false;
  pos++;

  int lo = (pos < len) ? hexVal(s[pos]) : -1;
  if (lo < 0) {
    out = (uint8_t)hi;  // single-digit form
  } else {
    out = (uint8_t)((hi << 4) | lo);
    pos++;
  }

  // Must be followed by whitespace or end of line, else it's a malformed token
  // like "0xff" or "123" that we'd otherwise silently truncate.
  if (pos < len && !isspace((unsigned char)s[pos])) return false;
  return true;
}

bool atEnd(const char *s, size_t len, size_t pos) {
  while (pos < len && isspace((unsigned char)s[pos])) pos++;
  return pos >= len;
}

}  // namespace

Command SerialProto::feed(char c) {
  if (c == '\n' || c == '\r') {
    if (len_ == 0 && !overflow_) return Command{};  // blank line, ignore

    Command cmd;
    if (overflow_) {
      cmd.kind = CommandKind::Error;
      cmd.error = "line too long";
    } else {
      cmd = parseLine();
    }
    len_ = 0;
    overflow_ = false;
    return cmd;
  }

  if (len_ < kMaxLine) {
    line_[len_++] = c;
  } else {
    overflow_ = true;  // keep draining until the terminator, then report once
  }
  return Command{};
}

Command SerialProto::parseLine() {
  Command cmd;
  size_t pos = 0;

  while (pos < len_ && isspace((unsigned char)line_[pos])) pos++;
  if (pos >= len_) return cmd;  // whitespace only

  const char verb = (char)toupper((unsigned char)line_[pos]);
  pos++;

  switch (verb) {
    case 'K': {
      uint8_t mod;
      if (!nextHexByte(line_, len_, pos, mod)) {
        cmd.kind = CommandKind::Error;
        cmd.error = "K needs 7 hex bytes: <mod> <k1..k6>";
        return cmd;
      }
      cmd.key.modifiers = mod;
      for (int i = 0; i < 6; i++) {
        if (!nextHexByte(line_, len_, pos, cmd.key.keys[i])) {
          cmd.kind = CommandKind::Error;
          cmd.error = "K needs 7 hex bytes: <mod> <k1..k6>";
          return cmd;
        }
      }
      if (!atEnd(line_, len_, pos)) {
        cmd.kind = CommandKind::Error;
        cmd.error = "K has trailing junk";
        return cmd;
      }
      cmd.kind = CommandKind::Key;
      return cmd;
    }

    case 'C': {
      if (!nextHexByte(line_, len_, pos, cmd.consumer.bytes[0]) ||
          !nextHexByte(line_, len_, pos, cmd.consumer.bytes[1])) {
        cmd.kind = CommandKind::Error;
        cmd.error = "C needs 2 hex bytes: <b0> <b1>";
        return cmd;
      }
      if (!atEnd(line_, len_, pos)) {
        cmd.kind = CommandKind::Error;
        cmd.error = "C has trailing junk";
        return cmd;
      }
      cmd.kind = CommandKind::Consumer;
      return cmd;
    }

    case 'R':
      cmd.kind = atEnd(line_, len_, pos) ? CommandKind::ReleaseAll : CommandKind::Error;
      if (cmd.kind == CommandKind::Error) cmd.error = "R takes no arguments";
      return cmd;

    case 'P':
      cmd.kind = atEnd(line_, len_, pos) ? CommandKind::Ping : CommandKind::Error;
      if (cmd.kind == CommandKind::Error) cmd.error = "P takes no arguments";
      return cmd;

    default:
      cmd.kind = CommandKind::Error;
      cmd.error = "unknown command (expected K, C, R or P)";
      return cmd;
  }
}
