#!/usr/bin/env python3
"""Self-test for send_bundle.expect() with a fake serial port (no device)."""
import sys
import time

sys.path.insert(0, "tools")
from send_bundle import expect


class Fake:
    def __init__(self, lines, delay=0.0):
        self._lines = [l.encode() + b"\n" for l in lines]
        self.timeout = 5
        self._delay = delay

    def readline(self):
        if self._delay:
            time.sleep(self._delay)
        if self._lines:
            return self._lines.pop(0)
        time.sleep(0.05)
        return b""


def check(name, fn):
    try:
        fn()
    except SystemExit as e:
        print(f"{name}: SystemExit({e})")
        return str(e)
    print(f"{name}: returned")
    return None


# 1. normal: boot logs then GO
s = Fake(["I (1328) transfer mode: waiting", "GO"])
assert expect(s, "GO") == "GO"
print("normal ok")

# 2. deadline exceeded (short sleeps to keep test fast)
r = check("deadline", lambda: expect(
    Fake([]), "GO", timeout=1, idle_limit=100,
    deadline=time.time() + 2))
assert r and "timeout" in r

# 3. reboot marker mid-transfer
r = check("reboot", lambda: expect(
    Fake(["TAB5_XFER_READY"], ), "ACK",
    fail_on=("TAB5_XFER_READY", "ESP-ROM:")))
assert r and "reset" in r

# 4. stall on silence
r = check("stall", lambda: expect(Fake([]), "ACK", timeout=1, idle_limit=3))
assert r and "stall" in r

print("all self-tests passed")
