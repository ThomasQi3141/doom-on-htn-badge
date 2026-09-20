"""Serial capture + boot-log parsing for the two-badge ESP-NOW Doom port.

Everything here is read-only with respect to the badge: the only line we ever
drive is RTS, which reboots the chip.  Nothing in this file flashes or erases.

Usage from the shell:

    source ~/esp/esp-idf/export.sh
    python3 tools/coop/check.py --gate boots

pyserial is only needed for the live-capture half; the parser half is pure
stdlib so selftest.py can exercise it with no hardware and no IDF env.
"""

import glob
import os
import re
import statistics
import time

# ---------------------------------------------------------------------------
# pyserial import
# ---------------------------------------------------------------------------
# pyserial is installed inside the ESP-IDF python virtualenv, not in the system
# python3.  A bare `import serial` from a shell that has not sourced export.sh
# dies with an ImportError that tells the user nothing useful.  Defer the
# failure to the point of use and make the message actionable instead.

SERIAL_IMPORT_ERROR = None
try:
    import serial  # type: ignore
except Exception as exc:  # pragma: no cover - depends on the caller's env
    serial = None
    SERIAL_IMPORT_ERROR = exc

PYSERIAL_HELP = (
    "pyserial is not importable from this interpreter.\n"
    "It lives in the ESP-IDF python environment, not the system python.\n"
    "Run:  source ~/esp/esp-idf/export.sh\n"
    "and then re-run this command."
)


class HarnessError(Exception):
    """Something went wrong that the user can act on."""


def require_serial():
    """Return the serial module or raise a human-readable error."""
    if serial is None:
        raise HarnessError(
            "%s\n(original error: %s)" % (PYSERIAL_HELP, SERIAL_IMPORT_ERROR)
        )
    return serial


# ---------------------------------------------------------------------------
# Port discovery
# ---------------------------------------------------------------------------

PORT_GLOBS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*")


def list_ports():
    found = []
    for pattern in PORT_GLOBS:
        found.extend(sorted(glob.glob(pattern)))
    return found


def find_port(timeout=20.0, poll=0.25):
    """Wait for a badge port to exist and return it.

    The badge is a USB Serial/JTAG peripheral *inside* the ESP32-C3, so the USB
    device itself disappears from the bus whenever the chip resets and comes
    back a moment later.  Anything that wants a port therefore has to be
    willing to wait rather than fail on the first look.

    In practice only one badge enumerates at a time, so we return a single port
    rather than pretending we can watch both ends of the link.
    """
    deadline = time.time() + timeout
    while True:
        ports = list_ports()
        if ports:
            return ports[0]
        if time.time() >= deadline:
            raise HarnessError(
                "no badge serial port found (looked for %s over %.0fs). "
                "Is the badge plugged in?" % (", ".join(PORT_GLOBS), timeout)
            )
        time.sleep(poll)


def open_port(port, baud=115200, timeout=30.0):
    """Open `port`, retrying while it re-enumerates.

    Opening a port that is mid-reboot fails with `[Errno 2] No such file or
    directory` (the device node is literally gone) or with a busy/permission
    error while the OS finishes attaching it.  Both are transient and expected
    -- NOT error conditions -- so back off and retry rather than aborting a
    whole test run over a race we caused ourselves by resetting the board.
    """
    ser = require_serial()
    deadline = time.time() + timeout
    delay = 0.2
    while True:
        try:
            handle = ser.Serial(port, baud, timeout=0.2)
            # Opening the port should not itself yank the reset lines around.
            try:
                handle.dtr = False
                handle.rts = False
            except Exception:
                pass
            return handle
        except Exception as exc:
            if time.time() >= deadline:
                raise HarnessError(
                    "could not open %s after %.0fs: %s" % (port, timeout, exc)
                )
            time.sleep(delay)
            delay = min(delay * 1.6, 1.5)
            # If the node vanished entirely the kernel may hand it back under a
            # slightly different name; re-resolve instead of spinning forever on
            # a path that is never coming back.
            if not os.path.exists(port):
                ports = list_ports()
                if ports:
                    port = ports[0]


def pulse_reset(handle, hold=0.12):
    """Reboot the badge by toggling RTS. The only line we ever drive."""
    try:
        handle.dtr = False
        handle.rts = True
        time.sleep(hold)
        handle.rts = False
    except Exception as exc:
        raise HarnessError("failed to toggle RTS on the badge: %s" % exc)


def reset_and_capture(port=None, seconds=12.0, baud=115200, reset=True, echo=False):
    """Reset the badge and collect serial lines for `seconds`.

    Returns a list of decoded lines.  Bytes that are not valid UTF-8 are
    replaced rather than raising: the first bytes after a reset are routinely
    garbage because host and chip disagree about the line state briefly.
    """
    if port is None:
        port = find_port()
    handle = open_port(port, baud=baud)
    lines = []
    buf = bytearray()
    try:
        if reset:
            try:
                handle.reset_input_buffer()
            except Exception:
                pass
            pulse_reset(handle)
        deadline = time.time() + seconds
        while time.time() < deadline:
            try:
                chunk = handle.read(4096)
            except Exception:
                # The device re-enumerated under us (firmware rebooted, or a
                # brownout dropped USB).  Try to reattach for the rest of the
                # window instead of losing the capture entirely.
                try:
                    handle.close()
                except Exception:
                    pass
                remaining = deadline - time.time()
                if remaining <= 0:
                    break
                try:
                    handle = open_port(port, baud=baud, timeout=remaining)
                except HarnessError:
                    break
                continue
            if not chunk:
                continue
            buf.extend(chunk)
            while b"\n" in buf:
                raw, _, rest = bytes(buf).partition(b"\n")
                buf = bytearray(rest)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                lines.append(line)
                if echo:
                    print(line, flush=True)
    finally:
        try:
            handle.close()
        except Exception:
            pass
    if buf:
        lines.append(bytes(buf).decode("utf-8", "replace").rstrip("\r"))
    return lines


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

RE_BANNER = re.compile(r"=== Doom on the badge ===")
RE_FREE_DRAM = re.compile(
    r"\[(?P<stage>[^\]]+)\]\s+free DRAM (?P<free>\d+), largest block (?P<largest>\d+)"
)
RE_RADIO_UP = re.compile(
    r"radio:\s*up on channel (?P<chan>\d+), mac (?P<mac>[0-9a-fA-F:]{17})"
)
RE_WAD = re.compile(r"WAD id (?P<wad>0x[0-9a-fA-F]+)")
RE_HOSTING = re.compile(r"hosting: waiting up to (?P<ms>\d+) ms for a partner")
RE_FALLBACK = re.compile(r"nobody joined; falling back to single player")
RE_PAIRED = re.compile(r"paired, I am player (?P<n>\d+) of (?P<total>\d+)")
RE_ZONE = re.compile(
    r"zone:\s*(?P<size>\d+) bytes \(largest block was (?P<largest>\d+),"
    r"\s*(?P<leftover>\d+) left over\)"
)
RE_GAMELOOP = re.compile(r"entering the game loop")
RE_FRAME = re.compile(
    r"frame (?P<frame>\d+):\s*(?P<fps>[\d.]+) fps, gametic (?P<gametic>\d+),"
    r"\s*zone free (?P<zonefree>\d+), buttons (?P<buttons>0x[0-9a-fA-F]+)"
)
RE_OVERFLOW = re.compile(
    r"renderer ran out:\s*visplanes (?P<visplanes>\d+), drawsegs (?P<drawsegs>\d+)"
)
RE_RADIO_STATS = re.compile(
    r"radio:\s*sent_ok (?P<sent_ok>\d+), failures (?P<failures>\d+),"
    r"\s*rx (?P<rx>\d+), dropped (?P<dropped>\d+)"
)
RE_IERROR_RECORDED = re.compile(r"last recorded I_Error:\s*(?P<msg>.*?)\s*\*\*\*")
RE_IERROR_LOOSE = re.compile(r"I_Error:\s*(?P<msg>.+)")
RE_ABORT = re.compile(r"abort\(\) was called at PC")
RE_BROWNOUT = re.compile(r"[Bb]rownout detector was triggered")
RE_PANIC = re.compile(
    r"Guru Meditation Error|StoreProhibited|LoadProhibited|IllegalInstruction"
)


class BootResult(object):
    """Structured view of one capture window."""

    def __init__(self):
        self.lines = []
        self.boot_count = 0          # "=== Doom on the badge ===" banners seen
        self.dram_stages = []        # [(stage, free, largest)] in log order
        self.radio_mac = None
        self.radio_channel = None
        self.wad_id = None
        self.hosting_ms = None
        self.pairing = None          # None | "fallback" | "paired"
        self.player_index = None
        self.player_total = None
        self.zone_size = None
        self.zone_largest = None
        self.zone_leftover = None
        self.zone_free = []          # samples from the frame lines
        self.fps = []
        self.gametics = []
        self.frames = []             # [(frame, fps, gametic, zone_free, buttons)]
        self.overflow_lines = 0
        self.visplane_overflows = 0
        self.drawseg_overflows = 0
        self.radio_samples = []      # [(sent_ok, failures, rx, dropped)]
        self.reached_game_loop = False
        self.i_errors = []
        self.aborted = False
        self.brownout = False
        self.panic = False

    # -- convenience -------------------------------------------------------
    @property
    def median_fps(self):
        return statistics.median(self.fps) if self.fps else None

    @property
    def min_zone_free(self):
        return min(self.zone_free) if self.zone_free else None

    @property
    def radio_up(self):
        return self.radio_mac is not None

    @property
    def crashed(self):
        return bool(self.i_errors) or self.aborted or self.panic or self.brownout

    def to_dict(self):
        return {
            "boot_count": self.boot_count,
            "dram_stages": [
                {"stage": s, "free": f, "largest": l} for (s, f, l) in self.dram_stages
            ],
            "radio_mac": self.radio_mac,
            "radio_channel": self.radio_channel,
            "wad_id": self.wad_id,
            "hosting_ms": self.hosting_ms,
            "pairing": self.pairing,
            "player_index": self.player_index,
            "player_total": self.player_total,
            "zone_size": self.zone_size,
            "zone_largest": self.zone_largest,
            "zone_leftover": self.zone_leftover,
            "zone_free_min": self.min_zone_free,
            "zone_free_samples": self.zone_free,
            "fps_samples": self.fps,
            "fps_median": self.median_fps,
            "gametics": self.gametics,
            "overflow_lines": self.overflow_lines,
            "visplane_overflows": self.visplane_overflows,
            "drawseg_overflows": self.drawseg_overflows,
            "radio_samples": [
                {"sent_ok": a, "failures": b, "rx": c, "dropped": d}
                for (a, b, c, d) in self.radio_samples
            ],
            "radio_up": self.radio_up,
            "reached_game_loop": self.reached_game_loop,
            "i_errors": self.i_errors,
            "aborted": self.aborted,
            "brownout": self.brownout,
            "panic": self.panic,
            "line_count": len(self.lines),
        }


def parse_lines(lines):
    """Turn a captured boot log into a BootResult. Pure function, no I/O."""
    r = BootResult()
    r.lines = list(lines)
    for line in r.lines:
        if RE_BANNER.search(line):
            r.boot_count += 1

        m = RE_FREE_DRAM.search(line)
        if m:
            r.dram_stages.append(
                (m.group("stage"), int(m.group("free")), int(m.group("largest")))
            )

        m = RE_RADIO_UP.search(line)
        if m:
            r.radio_mac = m.group("mac").lower()
            r.radio_channel = int(m.group("chan"))

        m = RE_WAD.search(line)
        if m:
            r.wad_id = m.group("wad").lower()

        m = RE_HOSTING.search(line)
        if m:
            r.hosting_ms = int(m.group("ms"))

        if RE_FALLBACK.search(line):
            r.pairing = "fallback"

        m = RE_PAIRED.search(line)
        if m:
            r.pairing = "paired"
            r.player_index = int(m.group("n"))
            r.player_total = int(m.group("total"))

        m = RE_ZONE.search(line)
        if m:
            r.zone_size = int(m.group("size"))
            r.zone_largest = int(m.group("largest"))
            r.zone_leftover = int(m.group("leftover"))

        if RE_GAMELOOP.search(line):
            r.reached_game_loop = True

        m = RE_FRAME.search(line)
        if m:
            fps = float(m.group("fps"))
            gametic = int(m.group("gametic"))
            zfree = int(m.group("zonefree"))
            r.fps.append(fps)
            r.gametics.append(gametic)
            r.zone_free.append(zfree)
            r.frames.append(
                (int(m.group("frame")), fps, gametic, zfree, m.group("buttons"))
            )

        m = RE_OVERFLOW.search(line)
        if m:
            r.overflow_lines += 1
            r.visplane_overflows += int(m.group("visplanes"))
            r.drawseg_overflows += int(m.group("drawsegs"))

        m = RE_RADIO_STATS.search(line)
        if m:
            r.radio_samples.append(
                (
                    int(m.group("sent_ok")),
                    int(m.group("failures")),
                    int(m.group("rx")),
                    int(m.group("dropped")),
                )
            )

        m = RE_IERROR_RECORDED.search(line)
        if m:
            r.i_errors.append(m.group("msg").strip())
        else:
            m = RE_IERROR_LOOSE.search(line)
            if m and "last recorded" not in line:
                r.i_errors.append(m.group("msg").strip())

        if RE_ABORT.search(line):
            r.aborted = True
        if RE_BROWNOUT.search(line):
            r.brownout = True
        if RE_PANIC.search(line):
            r.panic = True
    return r


def parse_text(text):
    return parse_lines(text.splitlines())
