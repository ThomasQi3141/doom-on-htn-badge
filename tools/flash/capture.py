#!/usr/bin/env python
"""Reset the badge and capture its USB Serial/JTAG console for N seconds."""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem2101"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

s = serial.Serial(port, 115200, timeout=0.2)

# USB Serial/JTAG reset: DTR drives the boot strap, RTS drives EN.
s.setDTR(False); s.setRTS(False); time.sleep(0.1)
s.setDTR(False); s.setRTS(True);  time.sleep(0.1)   # hold in reset
s.setDTR(False); s.setRTS(False); time.sleep(0.1)   # release -> boots to app
s.reset_input_buffer()

end = time.time() + secs
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
