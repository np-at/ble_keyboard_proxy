#!/usr/bin/env python3
"""Tests for the pure parts of proxy_term: key resolution and escape decoding.

These need no hardware and no phone — they cover exactly the layer where a bug is
silent, because the firmware answering SENT tells you nothing about whether the
right usage code was chosen. Anything involving the serial link or a real terminal
is out of scope here and has to be exercised by hand.

    ~/.platformio/penv/bin/python host/test_proxy_term.py           # all
    ~/.platformio/penv/bin/python host/test_proxy_term.py escape    # matching only
"""

import sys

import proxy_term as pt


def feeder(s):
    """Turn a string into a read1() callable, returning "" once exhausted."""
    it = iter(s)
    return lambda: next(it, "")


CASES = []


def case(name):
    def deco(fn):
        CASES.append((name, fn))
        return fn
    return deco


@case("Shift+Tab (CSI Z) decodes to Shift held with Tab")
def test_shift_tab():
    # The reported bug: this used to fall through and send a bare Escape.
    assert pt.read_escape(feeder("[Z")) == (pt.MOD_LSHIFT, pt.KEY_TAB)


@case("plain Tab is unshifted, and distinct from Shift+Tab")
def test_plain_tab():
    assert pt.resolve_char("\t") == (pt.MOD_NONE, pt.KEY_TAB)
    assert pt.resolve_char("\t") != pt.read_escape(feeder("[Z"))


@case("arrow keys still decode after the tuple refactor")
def test_arrows():
    assert pt.read_escape(feeder("[A")) == (pt.MOD_NONE, 0x52)
    assert pt.read_escape(feeder("[B")) == (pt.MOD_NONE, 0x51)
    assert pt.read_escape(feeder("[C")) == (pt.MOD_NONE, 0x4F)
    assert pt.read_escape(feeder("[D")) == (pt.MOD_NONE, 0x50)


@case("multi-byte sequences (page up, forward delete) decode")
def test_multibyte():
    assert pt.read_escape(feeder("[5~")) == (pt.MOD_NONE, 0x4B)
    assert pt.read_escape(feeder("[3~")) == (pt.MOD_NONE, 0x4C)


@case("modified CSI form carries its modifier")
def test_csi_modified():
    # ESC [ 1 ; 5 C == Ctrl+Right; n-1 == 4 == ctrl bit.
    assert pt.read_escape(feeder("[1;5C")) == (pt.MOD_LCTRL, 0x4F)
    # n-1 == 1 == shift bit.
    assert pt.read_escape(feeder("[1;2D")) == (pt.MOD_LSHIFT, 0x50)
    # n-1 == 3 == shift|alt, so both bits must survive.
    assert pt.read_escape(feeder("[1;4A")) == (pt.MOD_LSHIFT | pt.MOD_LALT, 0x52)


@case("unknown escape returns None so the caller sends a bare Escape")
def test_unknown_escape():
    assert pt.read_escape(feeder("[9999x")) is None
    assert pt.read_escape(feeder("")) is None


@case("printable ASCII all resolves, with shift where required")
def test_printable():
    assert pt.resolve_char("a") == (pt.MOD_NONE, 0x04)
    assert pt.resolve_char("A") == (pt.MOD_LSHIFT, 0x04)
    assert pt.resolve_char("?") == (pt.MOD_LSHIFT, 0x38)
    for ch in map(chr, range(0x20, 0x7F)):
        assert pt.resolve_char(ch) is not None, f"unmapped printable {ch!r}"


@case("every target defines every command, with a valid action shape")
def test_targets_complete():
    for target, table in pt.TARGETS.items():
        for name, _label in pt.COMMANDS.values():
            assert name in table, f"{target} missing command {name}"
            kind, a, b = table[name]
            assert kind in ("K", "C", "X"), f"{target}/{name} bad kind {kind!r}"
            if kind == "X":
                # An unavailable action must explain itself; the reason is shown
                # to the user in place of sending anything.
                assert isinstance(a, str) and a.strip(), f"{target}/{name} no reason"
            else:
                assert 0 <= a <= 0xFF and 0 <= b <= 0xFF, f"{target}/{name} byte range"


@case("iPhone reports Back and app switcher as unavailable rather than sending")
def test_iphone_unavailable():
    # Verified on hardware: iOS ignores AC Back, and Cmd+[/Cmd+Tab are iPad-only.
    # Sending them anyway would look like a silent failure.
    assert pt.TARGETS["iphone"]["back"][0] == "X"
    assert pt.TARGETS["iphone"]["apps"][0] == "X"
    # ...but the two that were verified working must still actually send.
    assert pt.TARGETS["iphone"]["home"] == ("C", 0x80, 0x00)
    assert pt.TARGETS["iphone"]["spot"] == ("K", pt.MOD_LGUI, pt.KEY_SPACE)


@case("command_help renders every target without blowing up")
def test_command_help():
    for target in pt.TARGETS:
        text = pt.command_help(target)
        assert target in text
        for _name, label in pt.COMMANDS.values():
            assert label in text, f"{target} help missing {label}"


@case("--probe specs parse, and bad ones are rejected")
def test_parse_probe():
    assert pt.parse_probe("K:08:2B") == ("K", 0x08, 0x2B)
    assert pt.parse_probe("c:00:20") == ("C", 0x00, 0x20)
    for bad in ("K:08", "Z:00:00", "K:zz:00", "K:100:00", ""):
        try:
            pt.parse_probe(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"{bad!r} should have been rejected")


@case("leader key is not something resolve_char would also map")
def test_leader_unambiguous():
    # If Ctrl-] resolved as a character too, the leader would eat a real keystroke.
    assert pt.resolve_char(pt.LEADER) is None


def main():
    # Optional substring filter: `test_proxy_term.py escape` runs matching cases only.
    pattern = sys.argv[1] if len(sys.argv) > 1 else ""
    cases = [(n, f) for n, f in CASES if pattern.lower() in n.lower()]
    if not cases:
        print(f"no test matches {pattern!r}")
        return 1

    failed = 0
    for name, fn in cases:
        try:
            fn()
        except AssertionError as e:
            failed += 1
            print(f"FAIL  {name}\n        {e}")
        except Exception as e:  # noqa: BLE001 - report any error as a failure
            failed += 1
            print(f"ERROR {name}\n        {type(e).__name__}: {e}")
        else:
            print(f"ok    {name}")
    print(f"\n{len(cases) - failed}/{len(cases)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
