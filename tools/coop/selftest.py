#!/usr/bin/env python3
"""Offline proof that the parser and the gates agree with reality.

No hardware, no pyserial, no ESP-IDF env: this feeds recorded boot logs through
badge.parse_text() and asserts that each gate fires exactly when it should.

    python3 tools/coop/selftest.py

Exits 0 when every case passes.  The sample logs below are transcribed from
real badge output; if the firmware's log format changes, this file is the first
thing that should break.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import badge  # noqa: E402
import check  # noqa: E402

# ---------------------------------------------------------------------------
# Sample logs
# ---------------------------------------------------------------------------

HEALTHY_SOLO = """\
ESP-ROM:esp32c3-api1-20210207
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac 68:ee:8f:01:07:ac
I (874) badge_net: WAD id 0xa6c7aa3a
I (874) badge_net: hosting: waiting up to 10000 ms for a partner
W (10886) badge_net: nobody joined; falling back to single player
I (10890) doom: [post-net] free DRAM 104512, largest block 86016
zone: 57344 bytes (largest block was 69632, 21536 left over)
I (925) doom: entering the game loop
I (3424) doom.plat: frame 60: 23.6 fps, gametic 88, zone free 30260, buttons 0x001
I (6424) doom.plat: frame 120: 24.1 fps, gametic 176, zone free 29880, buttons 0x000
I (9424) doom.plat: frame 180: 22.9 fps, gametic 264, zone free 30104, buttons 0x004
I (11042) doom: radio: sent_ok 50, failures 0, rx 8, dropped 0
I (12424) doom.plat: frame 240: 23.4 fps, gametic 352, zone free 29944, buttons 0x000
I (14042) doom: radio: sent_ok 118, failures 0, rx 20, dropped 0
"""

PAIRED = """\
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac e8:3d:c1:20:cf:cc
I (874) badge_net: WAD id 0xa6c7aa3a
I (874) badge_net: hosting: waiting up to 10000 ms for a partner
I (1902) badge_net: paired, I am player 1 of 2
zone: 49152 bytes (largest block was 61440, 18304 left over)
I (1990) doom: entering the game loop
I (4490) doom.plat: frame 60: 21.7 fps, gametic 84, zone free 24180, buttons 0x000
I (7490) doom.plat: frame 120: 20.9 fps, gametic 168, zone free 23960, buttons 0x002
I (10490) doom.plat: frame 180: 21.2 fps, gametic 252, zone free 24020, buttons 0x000
I (11042) doom: radio: sent_ok 340, failures 0, rx 336, dropped 0
I (14042) doom: radio: sent_ok 690, failures 0, rx 681, dropped 0
"""

CRASH_LOOP = """\
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac 68:ee:8f:01:07:ac
I (874) badge_net: WAD id 0xa6c7aa3a
zone: 57344 bytes (largest block was 69632, 21536 left over)
I (925) doom: entering the game loop
I (1424) doom.plat: frame 60: 18.2 fps, gametic 88, zone free 1180, buttons 0x000
*** last recorded I_Error: Z_Malloc: failed on allocation of 10264 bytes ***
abort() was called at PC 0x42004aa1 on core 0
ESP-ROM:esp32c3-api1-20210207
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac 68:ee:8f:01:07:ac
zone: 57344 bytes (largest block was 69632, 21536 left over)
I (925) doom: entering the game loop
*** last recorded I_Error: Z_Malloc: failed on allocation of 10264 bytes ***
abort() was called at PC 0x42004aa1 on core 0
"""

# Boots, runs, but the renderer keeps blowing its limits and fps is on the floor.
SLOW_AND_OVERFLOWING = """\
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac 68:ee:8f:01:07:ac
I (874) badge_net: WAD id 0xa6c7aa3a
I (874) badge_net: hosting: waiting up to 10000 ms for a partner
W (10886) badge_net: nobody joined; falling back to single player
zone: 40960 bytes (largest block was 53248, 12288 left over)
I (925) doom: entering the game loop
W (3400) doom.plat:   renderer ran out: visplanes 3, drawsegs 0 -- dropping the frame
I (3424) doom.plat: frame 60: 12.6 fps, gametic 40, zone free 1880, buttons 0x000
W (6300) doom.plat:   renderer ran out: visplanes 7, drawsegs 2 -- dropping the frame
I (6424) doom.plat: frame 120: 13.1 fps, gametic 82, zone free 1740, buttons 0x000
"""

# Radio hardware came up but every unicast is failing.
RADIO_FAILING = """\
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac 68:ee:8f:01:07:ac
I (874) badge_net: WAD id 0xa6c7aa3a
I (1902) badge_net: paired, I am player 2 of 2
zone: 49152 bytes (largest block was 61440, 18304 left over)
I (1990) doom: entering the game loop
I (4490) doom.plat: frame 60: 22.0 fps, gametic 84, zone free 24180, buttons 0x000
I (11042) doom: radio: sent_ok 12, failures 44, rx 0, dropped 0
I (14042) doom: radio: sent_ok 12, failures 96, rx 0, dropped 0
"""

# Never gets as far as the game loop; the log just stops after the radio.
STALLED = """\
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
I (217) radio: up on channel 1, mac 68:ee:8f:01:07:ac
I (874) badge_net: WAD id 0xa6c7aa3a
I (874) badge_net: hosting: waiting up to 10000 ms for a partner
"""

BROWNOUT = """\
=== Doom on the badge ===
I (157) doom: [boot] free DRAM 110800, largest block 90112
zone: 57344 bytes (largest block was 69632, 21536 left over)
I (925) doom: entering the game loop
I (3424) doom.plat: frame 60: 23.6 fps, gametic 88, zone free 30260, buttons 0x001
Brownout detector was triggered
"""

# Real captures start mid-line and contain replacement characters where the
# UART was still settling.  The parser must not choke on that.
GARBLED_PREFIX = "��o�\x00garbage" + HEALTHY_SOLO


# ---------------------------------------------------------------------------
# Expectations: (log name, log text, {gate: expected pass/fail})
# ---------------------------------------------------------------------------

CASES = [
    ("healthy-solo", HEALTHY_SOLO, {
        "boots": True,
        "solo": True,
        "memory": True,
        "radio": True,
        "pair-timeout": True,
        "paired": False,        # this badge never found a partner
        "no-overflow": True,
        "stable": True,
    }),
    ("paired", PAIRED, {
        "boots": True,
        "solo": True,
        "memory": True,
        "radio": True,
        "pair-timeout": False,  # it paired instead of timing out
        "paired": True,
        "no-overflow": True,
        "stable": True,
    }),
    ("crash-loop", CRASH_LOOP, {
        "boots": False,
        "solo": False,
        "memory": False,        # zone free dipped to 1180 -- under the 2000 floor
        "radio": False,         # no counter lines at all
        "pair-timeout": False,  # never announced hosting
        "paired": False,
        "no-overflow": True,    # it died before any overflow
        "stable": False,        # two banners == it rebooted
    }),
    ("slow-and-overflowing", SLOW_AND_OVERFLOWING, {
        "boots": True,
        "solo": False,          # median 12.85 fps
        "memory": False,        # zone 40960 < 45000
        "radio": False,         # radio up but no counters
        "pair-timeout": True,
        "paired": False,
        "no-overflow": False,
        "stable": True,
    }),
    ("radio-failing", RADIO_FAILING, {
        "boots": True,
        "solo": True,
        "memory": True,
        "radio": False,         # 96 failures
        "pair-timeout": False,
        "paired": True,
        "no-overflow": True,
        "stable": True,
    }),
    ("stalled", STALLED, {
        "boots": False,
        "solo": False,
        "memory": False,
        "radio": False,
        "pair-timeout": False,
        "paired": False,
        "no-overflow": True,
        "stable": False,
    }),
    ("brownout", BROWNOUT, {
        "boots": False,
        "solo": False,
        "memory": True,
        "radio": False,
        "pair-timeout": False,
        "paired": False,
        "no-overflow": True,
        "stable": False,
    }),
    ("garbled-prefix", GARBLED_PREFIX, {
        "boots": True,
        "solo": True,
        "memory": True,
        "radio": True,
        "no-overflow": True,
        "stable": True,
    }),
]


# ---------------------------------------------------------------------------
# Parser field checks: these pin the regexes, not just the gate verdicts.
# ---------------------------------------------------------------------------

def check_parser_fields(fail):
    r = badge.parse_text(HEALTHY_SOLO)
    expect = [
        ("boot_count", r.boot_count, 1),
        ("radio_mac", r.radio_mac, "68:ee:8f:01:07:ac"),
        ("radio_channel", r.radio_channel, 1),
        ("wad_id", r.wad_id, "0xa6c7aa3a"),
        ("hosting_ms", r.hosting_ms, 10000),
        ("pairing", r.pairing, "fallback"),
        ("zone_size", r.zone_size, 57344),
        ("zone_largest", r.zone_largest, 69632),
        ("zone_leftover", r.zone_leftover, 21536),
        ("zone_free_min", r.min_zone_free, 29880),
        ("fps_count", len(r.fps), 4),
        ("gametic_last", r.gametics[-1], 352),
        ("reached_game_loop", r.reached_game_loop, True),
        ("dram_stage_count", len(r.dram_stages), 2),
        ("dram_stage0", r.dram_stages[0], ("boot", 110800, 90112)),
        ("dram_stage1", r.dram_stages[1], ("post-net", 104512, 86016)),
        ("radio_samples", r.radio_samples, [(50, 0, 8, 0), (118, 0, 20, 0)]),
        ("crashed", r.crashed, False),
    ]
    for name, got, want in expect:
        if got != want:
            fail("healthy-solo parser field %s: got %r, want %r" % (name, got, want))

    p = badge.parse_text(PAIRED)
    for name, got, want in [
        ("pairing", p.pairing, "paired"),
        ("player_index", p.player_index, 1),
        ("player_total", p.player_total, 2),
        ("radio_mac", p.radio_mac, "e8:3d:c1:20:cf:cc"),
    ]:
        if got != want:
            fail("paired parser field %s: got %r, want %r" % (name, got, want))

    c = badge.parse_text(CRASH_LOOP)
    if c.boot_count != 2:
        fail("crash-loop boot_count: got %r, want 2" % c.boot_count)
    if not c.aborted:
        fail("crash-loop should set aborted")
    if c.i_errors != ["Z_Malloc: failed on allocation of 10264 bytes"] * 2:
        fail("crash-loop i_errors: got %r" % (c.i_errors,))

    o = badge.parse_text(SLOW_AND_OVERFLOWING)
    if (o.overflow_lines, o.visplane_overflows, o.drawseg_overflows) != (2, 10, 2):
        fail("overflow counters: got %r" % (
            (o.overflow_lines, o.visplane_overflows, o.drawseg_overflows),))

    b = badge.parse_text(BROWNOUT)
    if not b.brownout:
        fail("brownout should be detected")

    # to_dict() must be JSON-serialisable, since --json leans on it.
    import json
    json.dumps(r.to_dict())

    # An empty capture must produce a result, not an exception.
    e = badge.parse_text("")
    if e.reached_game_loop or e.crashed or e.fps:
        fail("empty log parsed into something non-empty")


def main():
    failures = []

    def fail(msg):
        failures.append(msg)

    check_parser_fields(fail)

    checked = 0
    for name, text, expectations in CASES:
        result = badge.parse_lines(text.splitlines())
        for gate_name, want_pass in sorted(expectations.items()):
            gate = check.GATES_BY_NAME[gate_name]
            got_pass, detail = gate.run(result)
            checked += 1
            mark = "ok  " if bool(got_pass) == want_pass else "BAD "
            print("%s %-22s %-13s %s (%s)" % (
                mark, name, gate_name,
                "pass" if got_pass else "fail", detail))
            if bool(got_pass) != want_pass:
                fail("%s/%s: expected %s, got %s -- %s" % (
                    name, gate_name,
                    "pass" if want_pass else "fail",
                    "pass" if got_pass else "fail", detail))

    # Every gate should be covered by at least one pass and one fail case,
    # otherwise a gate could be stuck-on or stuck-off and nobody would notice.
    for gate in check.GATES:
        passes = any(exp.get(gate.name) is True for _, _, exp in CASES)
        fails = any(exp.get(gate.name) is False for _, _, exp in CASES)
        if not passes:
            fail("gate %s has no passing sample" % gate.name)
        if not fails:
            fail("gate %s has no failing sample" % gate.name)

    print("")
    print("%d gate assertions over %d sample logs" % (checked, len(CASES)))
    if failures:
        print("FAILED (%d):" % len(failures))
        for f in failures:
            print("  - " + f)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
