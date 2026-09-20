# tools/coop — badge test harness

A repeatable replacement for "hand-roll a python serial reader, pipe it through
grep, squint at it". Resets a badge over RTS, captures its boot log for a
window, parses it, and asserts named pass/fail **gates**. Exits non-zero when a
gate fails, so it can be wired into a step's acceptance check.

This harness is **read-only on the hardware**. The only line it ever drives is
RTS (to reboot). It never flashes, never erases, and never calls esptool to
write anything.

## Setup

pyserial lives in the ESP-IDF python environment, not in system python, so:

```sh
source ~/esp/esp-idf/export.sh
python3 tools/coop/check.py --gate boots
```

If you forget, you get a message telling you to source `export.sh` rather than
a bare `ImportError`.

## Running

```sh
# default: 12 second window, 'boots' gate, autodetected port
python3 tools/coop/check.py

# several gates, longer window, explicit port
python3 tools/coop/check.py --gate boots --gate solo --gate memory \
    --seconds 20 --port /dev/cu.usbmodem101

# machine-readable: gate verdicts plus every parsed field
python3 tools/coop/check.py --gate solo --json

# keep the raw capture for post-mortem
python3 tools/coop/check.py --gate stable --seconds 60 --save-log /tmp/run.log

# re-check a saved log without touching hardware
python3 tools/coop/check.py --gate memory --from-log /tmp/run.log

# watch a badge that is already running, without rebooting it
python3 tools/coop/check.py --gate paired --no-reset --echo

python3 tools/coop/check.py --list-gates
```

Exit codes: `0` all gates passed, `1` a gate failed, `2` harness problem
(no port, no pyserial, bad arguments).

## Gates

| gate | passes when |
| --- | --- |
| `boots` | reaches `entering the game loop`, with no `I_Error`, abort, panic or brownout |
| `solo` | `boots`, and the median of the `fps` samples in the window is >= 20 |
| `memory` | `zone:` line reports >= 45000 bytes, and `zone free` never drops below 2000 |
| `radio` | `radio: up on channel N, mac ...` appeared, `sent_ok` increases across samples, `failures` == 0 |
| `pair-timeout` | host announced `hosting: waiting up to N ms`, then `nobody joined; falling back to single player`, then reached the game loop cleanly |
| `paired` | `paired, I am player N of 2` appeared with a plausible N |
| `no-overflow` | zero `renderer ran out: visplanes ..., drawsegs ...` lines in the window |
| `stable` | exactly one `=== Doom on the badge ===` banner (i.e. no reboot) and no crash over the window |

`pair-timeout` and `paired` are mutually exclusive by design: one asserts the
solo fallback path, the other asserts the co-op path.

Thresholds are constants at the top of `check.py`
(`MIN_MEDIAN_FPS`, `MIN_ZONE_SIZE`, `MIN_ZONE_FREE`).

## Two-badge procedure

Only **one** badge enumerates on USB at a time in practice, so only one badge
can be monitored. The other one has to be judged through the monitored badge's
`rx` / `dropped` counters and its `paired` line.

1. Plug in badge 1 (MAC `e8:3d:c1:20:cf:cc`). Flash it with the normal flash
   script — **not** with this harness.
2. Unplug it. Plug in badge 2 (MAC `68:ee:8f:01:07:ac`). Flash it.
3. Power both badges within the 10 second pairing window: the firmware hosts
   for 10000 ms and then falls back to single player, so the second badge has
   to be alive before that expires. In practice: hold both reset buttons, or
   power one from a battery and reset the USB-attached one immediately after.
4. With badge 2 still on USB, run the co-op gates against it:

   ```sh
   python3 tools/coop/check.py --gate paired --gate radio --gate stable \
       --seconds 25 --save-log /tmp/coop.log
   ```

   Note that `check.py` resets the badge it is attached to, which restarts that
   badge's 10-second host window — so the partner must be powered on (or reset)
   right after the harness starts. Use `--no-reset` if you would rather bring
   both up by hand and just observe.

Single-badge sanity, no partner needed:

```sh
python3 tools/coop/check.py --gate pair-timeout --gate solo --gate memory --seconds 20
```

### Port re-enumeration

The badge's serial port is a USB Serial/JTAG peripheral *inside* the ESP32-C3,
so the USB device disappears from the bus on every reset and comes back a
moment later. `[Errno 2] No such file or directory` when opening the port is
therefore normal, not a failure — `badge.open_port()` backs off and retries,
and re-resolves the port name if the kernel hands it back under a different
one. The same retry covers a badge that reboots mid-capture.

## Files

- `badge.py` — library: `find_port()`, `open_port()`, `reset_and_capture()`,
  and `parse_lines()` / `parse_text()` returning a `BootResult`
  (DRAM per stage, radio MAC/channel, WAD id, zone size and free samples, fps
  and gametic samples, pairing outcome, overflow counts, radio counters, and
  any I_Error / abort / panic / brownout). The parser is pure stdlib, so it
  works without pyserial.
- `check.py` — the gate CLI.
- `selftest.py` — offline proof. Feeds recorded sample logs (healthy solo,
  paired, crash loop, slow-and-overflowing, failing radio, stalled boot,
  brownout, garbled UTF-8 prefix) through the parser and asserts every gate
  fires exactly when it should, plus that each gate has at least one passing
  and one failing sample. No hardware and no IDF env required:

  ```sh
  python3 tools/coop/selftest.py
  ```

  Run this after any change to the parser, and after any firmware change to the
  log format — it is the first thing that should break.
