#!/usr/bin/env python3
"""Run a capture against a badge and assert pass/fail gates.

    source ~/esp/esp-idf/export.sh
    python3 tools/coop/check.py --gate boots --gate solo --seconds 15

Exits 0 if every requested gate passed, 1 if any failed, 2 on a harness
problem (no port, no pyserial, bad arguments).  Never writes flash.
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import badge  # noqa: E402

EXIT_OK = 0
EXIT_GATE_FAILED = 1
EXIT_HARNESS = 2

# Thresholds live here so a gate's numbers are readable in one place.
MIN_MEDIAN_FPS = 20.0
MIN_ZONE_SIZE = 45000
MIN_ZONE_FREE = 2000


class Gate(object):
    def __init__(self, name, help_text, fn):
        self.name = name
        self.help = help_text
        self.fn = fn

    def run(self, r):
        """Return (passed, detail)."""
        try:
            return self.fn(r)
        except Exception as exc:  # a parser surprise is a gate failure, not a crash
            return False, "gate raised %s: %s" % (type(exc).__name__, exc)


def _crash_reasons(r):
    reasons = []
    if r.i_errors:
        reasons.append("I_Error: " + "; ".join(r.i_errors))
    if r.aborted:
        reasons.append("abort() was called")
    if r.panic:
        reasons.append("cpu panic / guru meditation")
    if r.brownout:
        reasons.append("brownout detector fired")
    return reasons


def gate_boots(r):
    reasons = _crash_reasons(r)
    if not r.reached_game_loop:
        reasons.insert(0, "never printed 'entering the game loop'")
    if reasons:
        return False, "; ".join(reasons)
    return True, "reached the game loop cleanly"


def gate_solo(r):
    ok, detail = gate_boots(r)
    if not ok:
        return False, detail
    if not r.fps:
        return False, "no frame/fps lines in the window -- is the frame log enabled?"
    med = r.median_fps
    if med < MIN_MEDIAN_FPS:
        return False, "median fps %.1f over %d samples, want >= %.0f" % (
            med, len(r.fps), MIN_MEDIAN_FPS
        )
    return True, "median fps %.1f over %d samples" % (med, len(r.fps))


def gate_memory(r):
    if r.zone_size is None:
        return False, "no 'zone: N bytes' line seen"
    if r.zone_size < MIN_ZONE_SIZE:
        return False, "zone is %d bytes, want >= %d" % (r.zone_size, MIN_ZONE_SIZE)
    if not r.zone_free:
        return False, "zone sized at %d but no 'zone free' samples to check" % r.zone_size
    low = r.min_zone_free
    if low < MIN_ZONE_FREE:
        return False, "zone free dipped to %d, floor is %d" % (low, MIN_ZONE_FREE)
    return True, "zone %d bytes, free floor %d over %d samples" % (
        r.zone_size, low, len(r.zone_free)
    )


def gate_radio(r):
    if not r.radio_up:
        return False, "radio never reported 'up on channel N, mac ...'"
    if not r.radio_samples:
        return False, "radio up on %s but no 'sent_ok/failures' counter lines" % r.radio_mac
    failures = max(s[1] for s in r.radio_samples)
    if failures:
        return False, "radio reported %d send failures" % failures
    first_sent = r.radio_samples[0][0]
    last_sent = r.radio_samples[-1][0]
    if len(r.radio_samples) == 1:
        if last_sent <= 0:
            return False, "only one counter sample and sent_ok is %d" % last_sent
        return True, "radio up on %s, sent_ok %d, 0 failures" % (r.radio_mac, last_sent)
    if last_sent <= first_sent:
        return False, "sent_ok did not increase (%d -> %d)" % (first_sent, last_sent)
    return True, "radio up on %s, sent_ok %d -> %d, 0 failures" % (
        r.radio_mac, first_sent, last_sent
    )


def gate_pair_timeout(r):
    if r.hosting_ms is None:
        return False, "never announced 'hosting: waiting up to N ms for a partner'"
    if r.pairing == "paired":
        return False, "expected a timeout but the badge paired as player %s of %s" % (
            r.player_index, r.player_total
        )
    if r.pairing != "fallback":
        return False, "announced hosting (%d ms) but never fell back to single player" % r.hosting_ms
    if not r.reached_game_loop:
        return False, "fell back to single player but never reached the game loop"
    crash = _crash_reasons(r)
    if crash:
        return False, "fell back but then: " + "; ".join(crash)
    return True, "hosted %d ms, fell back to single player, game loop running" % r.hosting_ms


def gate_paired(r):
    if r.pairing != "paired":
        if r.pairing == "fallback":
            return False, "no partner joined; fell back to single player"
        return False, "no 'paired, I am player N of 2' line seen"
    if r.player_total != 2:
        return False, "paired but as player %s of %s, expected of 2" % (
            r.player_index, r.player_total
        )
    if r.player_index not in (1, 2):
        return False, "implausible player index %s" % r.player_index
    return True, "paired as player %d of %d" % (r.player_index, r.player_total)


def gate_no_overflow(r):
    if r.overflow_lines:
        return False, "%d 'renderer ran out' lines (visplanes %d, drawsegs %d)" % (
            r.overflow_lines, r.visplane_overflows, r.drawseg_overflows
        )
    return True, "no renderer overflow lines"


def gate_stable(r):
    crash = _crash_reasons(r)
    if crash:
        return False, "; ".join(crash)
    if r.boot_count > 1:
        return False, "badge rebooted: saw %d '=== Doom on the badge ===' banners" % r.boot_count
    if not r.reached_game_loop:
        return False, "never reached the game loop"
    return True, "one boot, no crash over the window"


GATES = [
    Gate("boots", "reaches the game loop with no I_Error and no abort", gate_boots),
    Gate("solo", "boots and holds a median of >= %.0f fps" % MIN_MEDIAN_FPS, gate_solo),
    Gate("memory", "zone >= %d bytes and zone free never below %d" % (MIN_ZONE_SIZE, MIN_ZONE_FREE), gate_memory),
    Gate("radio", "radio comes up, sent_ok increases, zero failures", gate_radio),
    Gate("pair-timeout", "host announces then falls back cleanly to single player", gate_pair_timeout),
    Gate("paired", "'paired, I am player N of 2' appears", gate_paired),
    Gate("no-overflow", "zero 'renderer ran out' lines in the window", gate_no_overflow),
    Gate("stable", "runs the whole window without rebooting", gate_stable),
]

GATES_BY_NAME = dict((g.name, g) for g in GATES)


def gate_help_text():
    width = max(len(g.name) for g in GATES)
    return "\n".join("  %-*s  %s" % (width, g.name, g.help) for g in GATES)


def build_parser():
    p = argparse.ArgumentParser(
        prog="check.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="gates:\n" + gate_help_text(),
    )
    p.add_argument(
        "--gate", action="append", dest="gates", metavar="NAME", default=None,
        help="gate to assert; repeatable. Default: boots",
    )
    p.add_argument("--seconds", type=float, default=12.0, help="capture window (default 12)")
    p.add_argument("--port", default=None, help="serial port; autodetected if omitted")
    p.add_argument("--json", action="store_true", help="machine-readable output on stdout")
    p.add_argument("--no-reset", action="store_true",
                   help="listen without toggling RTS (use when the badge is already running)")
    p.add_argument("--echo", action="store_true", help="stream captured lines to stderr as they arrive")
    p.add_argument("--save-log", metavar="PATH", default=None, help="write the raw capture to PATH")
    p.add_argument("--from-log", metavar="PATH", default=None,
                   help="parse an existing log file instead of touching hardware")
    p.add_argument("--list-gates", action="store_true", help="print the gate names and exit")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.list_gates:
        print(gate_help_text())
        return EXIT_OK

    names = args.gates or ["boots"]
    unknown = [n for n in names if n not in GATES_BY_NAME]
    if unknown:
        sys.stderr.write(
            "unknown gate(s): %s\nknown gates:\n%s\n" % (", ".join(unknown), gate_help_text())
        )
        return EXIT_HARNESS

    # --- capture ---------------------------------------------------------
    try:
        if args.from_log:
            with open(args.from_log, "r", errors="replace") as fh:
                lines = fh.read().splitlines()
            port = args.from_log
        else:
            port = args.port or badge.find_port()
            lines = badge.reset_and_capture(
                port,
                seconds=args.seconds,
                reset=not args.no_reset,
                echo=args.echo,
            )
    except badge.HarnessError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc), "gates": []}, indent=2))
        else:
            sys.stderr.write("harness error: %s\n" % exc)
        return EXIT_HARNESS
    except KeyboardInterrupt:
        sys.stderr.write("interrupted\n")
        return EXIT_HARNESS

    if args.save_log:
        with open(args.save_log, "w") as fh:
            fh.write("\n".join(lines) + "\n")

    result = badge.parse_lines(lines)

    # --- gates -----------------------------------------------------------
    reports = []
    all_ok = True
    for name in names:
        passed, detail = GATES_BY_NAME[name].run(result)
        all_ok = all_ok and passed
        reports.append({"gate": name, "passed": bool(passed), "detail": detail})

    if args.json:
        print(json.dumps({
            "ok": all_ok,
            "port": port,
            "seconds": args.seconds,
            "gates": reports,
            "parsed": result.to_dict(),
        }, indent=2, sort_keys=True))
    else:
        print("port %s, %d lines captured" % (port, len(lines)))
        for rep in reports:
            print("%s %-13s %s" % ("PASS" if rep["passed"] else "FAIL",
                                   rep["gate"], rep["detail"]))
        if not all_ok:
            failed = [r["gate"] for r in reports if not r["passed"]]
            sys.stderr.write("\nFAILED: %s\n" % ", ".join(failed))
            if args.save_log:
                sys.stderr.write("raw log: %s\n" % args.save_log)

    return EXIT_OK if all_ok else EXIT_GATE_FAILED


if __name__ == "__main__":
    sys.exit(main())
