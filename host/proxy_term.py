#!/usr/bin/env python3
"""Proxy keystrokes from this terminal to the BLE keyboard device.

Puts the terminal in raw mode, reads what you type, and forwards each keystroke to
the ESP32 as a HID report. Whatever you type here appears on the BLE-paired phone.

    python3 host/proxy_term.py            # auto-detect the port
    python3 host/proxy_term.py --port /dev/cu.usbmodem5C4C0629821
    python3 host/proxy_term.py --target ipad

Ctrl-] is a leader key for device commands, not an immediate quit:

    Ctrl-] h   Home          Ctrl-] q   Quit
    Ctrl-] b   Back          Ctrl-] ?   List commands
    Ctrl-] a   App switcher

Which chord actually does these things differs per OS, so pick --target
(android, ipad, iphone). See TARGETS below — those mappings are unverified
guesses and are expected to need adjustment against a real device.

Needs pyserial. If your system python lacks it, the PlatformIO venv has it:

    ~/.platformio/penv/bin/python host/proxy_term.py

IMPORTANT — close the PlatformIO serial monitor first. It and this script cannot both
hold the port, and the failure looks like the firmware hanging rather than a conflict.

LIMITATION worth understanding: a terminal in raw mode delivers *characters*, not key
up/down events. So this taps each key (press, release) and cannot express a genuinely
held key, real auto-repeat, or chords beyond the modifiers implied by the character
itself. Key-repeat you see on the phone is the phone's own doing, driven by the stream
of taps. Faithful hold/repeat needs OS-level event capture (a macOS CGEventTap), which
is the next milestone — the firmware protocol already supports it, this input source
doesn't.
"""

import argparse
import glob
import sys
import termios
import tty

try:
    import serial
except ImportError:
    sys.exit(
        "pyserial not found. Try:\n"
        "  ~/.platformio/penv/bin/python host/proxy_term.py"
    )

# --- HID usage codes -------------------------------------------------------------
# Full table at https://usb.org/sites/default/files/hut1_5.pdf section 10 (Keyboard page)

MOD_NONE = 0x00
MOD_LCTRL = 0x01
MOD_LSHIFT = 0x02
MOD_LALT = 0x04
MOD_LGUI = 0x08  # Command on Apple, Meta/Search on Android

KEY_ENTER = 0x28
KEY_ESC = 0x29
KEY_BACKSPACE = 0x2A
KEY_TAB = 0x2B
KEY_SPACE = 0x2C

# Characters reachable without shift: char -> usage code
UNSHIFTED = {
    **{c: 0x04 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")},
    **{c: 0x1E + i for i, c in enumerate("123456789")},
    "0": 0x27,
    "\n": KEY_ENTER,
    "\r": KEY_ENTER,
    "\t": KEY_TAB,
    " ": KEY_SPACE,
    "\x7f": KEY_BACKSPACE,  # macOS Delete sends DEL, not BS
    "\x08": KEY_BACKSPACE,
    "\x1b": KEY_ESC,
    "-": 0x2D, "=": 0x2E, "[": 0x2F, "]": 0x30, "\\": 0x31,
    ";": 0x33, "'": 0x34, "`": 0x35, ",": 0x36, ".": 0x37, "/": 0x38,
}

# Characters needing shift: char -> usage code of the unshifted key on the same cap.
# Uppercase letters are added below; do NOT add lowercase here. A char landing in both
# tables is a latent bug — it only resolves correctly because resolve_char() happens to
# consult UNSHIFTED first, so a reordering would silently capitalize everything.
SHIFTED = {
    "!": 0x1E, "@": 0x1F, "#": 0x20, "$": 0x21, "%": 0x22,
    "^": 0x23, "&": 0x24, "*": 0x25, "(": 0x26, ")": 0x27,
    "_": 0x2D, "+": 0x2E, "{": 0x2F, "}": 0x30, "|": 0x31,
    ":": 0x33, '"': 0x34, "~": 0x35, "<": 0x36, ">": 0x37, "?": 0x38,
}
# Uppercase letters map to their lowercase key with shift held.
SHIFTED.update({c.upper(): 0x04 + i for i, c in enumerate("abcdefghijklmnopqrstuvwxyz")})

_overlap = set(UNSHIFTED) & set(SHIFTED)
assert not _overlap, f"chars in both key tables (shift would be wrong): {sorted(_overlap)}"

# Terminal escape sequences (arrows, home/end, etc.) -> (modifiers, usage code).
# Read after an ESC; if the follow-on bytes don't match, we send a bare Escape.
#
# Values are (mod, usage) rather than a bare usage because some sequences carry a
# modifier of their own: Shift+Tab arrives as CSI Z, which is Shift held with Tab.
# Encoding that needed the modifier slot, and having it also lets the CSI-modifier
# form below (Ctrl+Right and friends) share this path.
ESCAPES = {
    "[A": (MOD_NONE, 0x52), "[B": (MOD_NONE, 0x51),
    "[C": (MOD_NONE, 0x4F), "[D": (MOD_NONE, 0x50),    # up down right left
    "[H": (MOD_NONE, 0x4A), "[F": (MOD_NONE, 0x4D),    # home end
    "[5~": (MOD_NONE, 0x4B), "[6~": (MOD_NONE, 0x4E),  # page up / page down
    "[3~": (MOD_NONE, 0x4C),                           # forward delete
    "[Z": (MOD_LSHIFT, KEY_TAB),                       # Shift+Tab (back-tab)
}

# Final byte of a CSI sequence -> usage, for the modified form ESC [ 1 ; <n> <letter>
# that terminals emit for Ctrl+Right, Shift+Home, etc. Without this every modified
# arrow silently degraded to a bare Escape.
CSI_FINAL = {"A": 0x52, "B": 0x51, "C": 0x4F, "D": 0x50, "H": 0x4A, "F": 0x4D}

# xterm encodes the modifier as (n - 1) with these bits.
CSI_MOD_BITS = ((1, MOD_LSHIFT), (2, MOD_LALT), (4, MOD_LCTRL))

LEADER = "\x1d"  # Ctrl-] — prefix for device commands (see COMMANDS)

# --- Device commands -------------------------------------------------------------
# Press LEADER then one of these keys to send a navigation command to the phone.
#
# An action is either ("K", mod, usage) — a keyboard chord — or ("C", b0, b1) — a
# consumer/media bitmask. The consumer form is limited to the 16 keys baked into
# T-vK's HID descriptor (see src/hid_report.h); b0/b1 are the KEY_MEDIA_* masks from
# the library header, so AC Home is (0x80, 0x00) and AC Back is (0x00, 0x20).
#
# !! THESE MAPPINGS ARE UNVERIFIED. !!
# Which chord a given OS honours over BLE HID is not something that can be checked
# from the host side — the firmware reporting SENT says nothing about what the phone
# did. Every entry below is a best-guess starting point that must be confirmed live
# on the device and edited when wrong. The iPhone and Android home/back entries are
# the shakiest; iPad's Cmd chords are the most likely to work as-is.

KEY_H = 0x0B
KEY_LEFTBRACKET = 0x2F

TARGETS = {
    # iPadOS has genuine hardware-keyboard support, so the Cmd chords are real.
    "ipad": {
        "home": ("K", MOD_LGUI, KEY_H),               # Cmd+H
        "back": ("K", MOD_LGUI, KEY_LEFTBRACKET),     # Cmd+[ — in-app back, not system
        "apps": ("K", MOD_LGUI, KEY_TAB),             # Cmd+Tab
    },
    # iPhone honours far fewer chords; lean on the consumer usage where one exists.
    "iphone": {
        "home": ("C", 0x80, 0x00),                    # AC Home
        "back": ("K", MOD_LGUI, KEY_LEFTBRACKET),     # Cmd+[ — in-app only, shaky
        "apps": ("K", MOD_LGUI, KEY_TAB),             # Cmd+Tab — may do nothing
    },
    # Android maps AC Home / AC Back to the real system buttons.
    "android": {
        "home": ("C", 0x80, 0x00),                    # AC Home
        "back": ("C", 0x00, 0x20),                    # AC Back
        "apps": ("K", MOD_LGUI, KEY_TAB),             # Meta+Tab — recents
    },
}

# Leader key -> (command name, human label). Command names index into TARGETS.
COMMANDS = {
    "h": ("home", "Home"),
    "b": ("back", "Back"),
    "a": ("apps", "App switcher"),
}


def parse_probe(spec):
    """Parse a --probe spec like 'K:08:2B' or 'C:00:20' into an action tuple."""
    parts = spec.split(":")
    if len(parts) != 3:
        raise ValueError(f"expected KIND:BYTE:BYTE, got {spec!r}")
    kind = parts[0].upper()
    if kind not in ("K", "C"):
        raise ValueError(f"kind must be K or C, got {parts[0]!r}")
    try:
        a, b = (int(p, 16) for p in parts[1:])
    except ValueError:
        raise ValueError(f"bytes must be hex, got {parts[1]!r} and {parts[2]!r}")
    if not (0 <= a <= 0xFF and 0 <= b <= 0xFF):
        raise ValueError("bytes must be in range 00..FF")
    return kind, a, b


def command_help(target):
    """One-line-per-command summary of the leader bindings for `target`."""
    lines = ["Ctrl-] commands:"]
    for key, (name, label) in sorted(COMMANDS.items()):
        kind = TARGETS[target][name][0]
        how = "chord" if kind == "K" else "consumer"
        lines.append(f"  Ctrl-] {key}   {label} ({how})")
    lines.append("  Ctrl-] q   Quit")
    lines.append("  Ctrl-] ?   This list")
    return "\n".join(lines)


def resolve_char(ch):
    """Map one character to (modifiers, usage). Returns None if unmappable."""
    if ch in UNSHIFTED:
        return MOD_NONE, UNSHIFTED[ch]
    if ch in SHIFTED:
        return MOD_LSHIFT, SHIFTED[ch]
    # Ctrl-A..Ctrl-Z arrive as 0x01..0x1A
    if "\x01" <= ch <= "\x1a":
        return MOD_LCTRL, 0x04 + (ord(ch) - 1)
    return None


class Link:
    """Serial link speaking the firmware's line protocol (see src/serial_proto.h)."""

    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.4)

    def _cmd(self, line):
        self.s.write((line + "\n").encode())
        self.s.flush()
        return self.s.readline().decode("utf-8", "replace").strip()

    def ping(self):
        return self._cmd("P")

    def tap(self, mod, usage):
        # Reports carry full state, so a tap is "this key held" then "nothing held".
        # The firmware answers SENT, not OK: it can confirm only that the report
        # reached the BLE stack, never that the phone applied it.
        self._cmd(f"K {mod:02X} {usage:02X} 00 00 00 00 00")
        self._cmd("K 00 00 00 00 00 00 00")

    def consumer(self, b0, b1):
        # Same tap shape as `tap`: assert the bitmask, then clear it. The consumer
        # report is state too, so without the clear the key reads as held forever.
        self._cmd(f"C {b0:02X} {b1:02X}")
        self._cmd("C 00 00")

    def send_action(self, action):
        """Dispatch a ("K", mod, usage) or ("C", b0, b1) device-command action."""
        kind, a, b = action
        if kind == "K":
            self.tap(a, b)
        elif kind == "C":
            self.consumer(a, b)
        else:
            raise ValueError(f"unknown action kind {kind!r}")

    def release_all(self):
        self._cmd("R")

    def close(self):
        try:
            self.release_all()
        finally:
            self.s.close()


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) or sorted(glob.glob("/dev/cu.usbserial*"))
    if not ports:
        sys.exit("No board port found. Is it plugged into the UART port?")
    return ports[0]


def parse_csi_modified(seq):
    """Parse the modified-key form ESC [ 1 ; <n> <final>, e.g. Ctrl+Right.

    Returns (mod, usage) or None. `seq` excludes the leading ESC.
    """
    if not seq.startswith("[1;") or len(seq) < 5:
        return None
    final = seq[-1]
    if final not in CSI_FINAL:
        return None
    try:
        n = int(seq[3:-1])
    except ValueError:
        return None
    mod = MOD_NONE
    for bit, flag in CSI_MOD_BITS:
        if (n - 1) & bit:
            mod |= flag
    return mod, CSI_FINAL[final]


def read_escape(read1):
    """After an ESC, try to consume a known escape sequence.

    Returns (mod, usage), or None if the bytes don't form a sequence we know —
    in which case the caller sends a bare Escape.
    """
    seq = ""
    for _ in range(6):  # longest handled form is "[1;10A"
        ch = read1()
        if not ch:
            break
        seq += ch
        if seq in ESCAPES:
            return ESCAPES[seq]
        modified = parse_csi_modified(seq)
        if modified is not None:
            return modified
        # Keep reading only while the bytes could still become something known.
        # "[1;" is the prefix of the modified form, which isn't in ESCAPES.
        prefix_of_known = any(k.startswith(seq) for k in ESCAPES)
        prefix_of_modified = "[1;".startswith(seq) or seq.startswith("[1;")
        if not (prefix_of_known or prefix_of_modified):
            break
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="serial port (default: auto-detect)")
    ap.add_argument("--text", default=None,
                    help="send this string and exit, instead of reading the terminal "
                         "(non-interactive; useful for scripting and for testing "
                         "without a TTY)")
    ap.add_argument("--probe", default=None, metavar="ACTION",
                    help="send one raw action and exit, for testing candidate "
                         "mappings without editing the table. Format 'K:mod:usage' "
                         "or 'C:b0:b1', hex, e.g. --probe K:08:2B (Cmd/Meta+Tab) "
                         "or --probe C:00:20 (AC Back). Repeatable via the shell.")
    ap.add_argument("--target", default="android", choices=sorted(TARGETS),
                    help="which device the Ctrl-] commands target; the chords for "
                         "Home/Back/app-switcher genuinely differ per OS "
                         "(default: android)")
    args = ap.parse_args()

    commands = TARGETS[args.target]

    port = args.port or find_port()
    link = Link(port)

    state = link.ping()
    print(f"port    : {port}")
    print(f"device  : {state}")
    if "connected" not in state:
        print("\nThe phone is NOT connected — keystrokes will be rejected until it pairs.")
        print("Pair with 'Proxy Keyboard' in Bluetooth settings, then rerun.")
        link.close()
        return 1

    if args.probe is not None:
        try:
            action = parse_probe(args.probe)
        except ValueError as e:
            link.close()
            sys.exit(f"bad --probe value: {e}")
        try:
            link.send_action(action)
        finally:
            link.close()
        kind, a, b = action
        print(f"probed {kind} {a:02X} {b:02X} — check the device, nothing is implied "
              f"by this exiting cleanly")
        return 0

    if args.text is not None:
        sent = unmapped = 0
        try:
            for ch in args.text:
                resolved = resolve_char(ch)
                if resolved is None:
                    unmapped += 1
                    continue
                link.tap(*resolved)
                sent += 1
        finally:
            link.close()
        print(f"sent {sent} keystrokes"
              + (f", {unmapped} unmapped and skipped" if unmapped else ""))
        return 0

    print(f"\ntarget  : {args.target}")
    print("\nTyping is now forwarded to the phone. Press Ctrl-] then q to quit.")
    print("(Keys are consumed here — they will NOT appear in this terminal.)")
    print(command_help(args.target) + "\n")

    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    sent = unmapped = 0
    quit_requested = False
    try:
        tty.setraw(fd)
        read1 = lambda: sys.stdin.read(1)
        while not quit_requested:
            ch = read1()
            if ch == LEADER:
                # Leader pressed: the next key selects a device command rather than
                # being forwarded. Unknown keys are dropped, not passed through, so
                # a mistyped command can't inject a stray keystroke into the phone.
                nxt = read1()
                if nxt == "q":
                    quit_requested = True
                elif nxt == "?":
                    # \r\n, not \n: the terminal is raw, so no ONLCR translation.
                    sys.stdout.write(command_help(args.target).replace("\n", "\r\n")
                                     + "\r\n")
                    sys.stdout.flush()
                elif nxt in COMMANDS:
                    name, label = COMMANDS[nxt]
                    link.send_action(commands[name])
                    sys.stdout.write(f"[sent {label}]\r\n")
                    sys.stdout.flush()
                    sent += 1
                else:
                    sys.stdout.write("\a")  # unknown command, beep
                    sys.stdout.flush()
                continue
            if ch == "\x1b":
                resolved = read_escape(read1)
                if resolved is not None:
                    link.tap(*resolved)
                else:
                    link.tap(MOD_NONE, KEY_ESC)
                sent += 1
                continue
            resolved = resolve_char(ch)
            if resolved is None:
                unmapped += 1
                continue
            link.tap(*resolved)
            sent += 1
    finally:
        # Restore the terminal before anything else, or the shell is left unusable.
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        link.close()

    print(f"\nDone. {sent} keystrokes forwarded"
          + (f", {unmapped} unmapped and skipped." if unmapped else "."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
