#!/usr/bin/env python3
"""Fake Arduino Nano on a pty. Answers PING with PONG, after an optional
startup delay that mimics the bootloader hand-off following a DTR reset."""
import os, pty, sys, time, select

boot_delay = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
answer     = (sys.argv[2] if len(sys.argv) > 2 else "yes") == "yes"

master, slave = pty.openpty()
name = os.ttyname(slave)
# The pty path is allocated at runtime, so hand it to the caller via a file
# they nominate (PORT_FILE), defaulting to ./port.txt next to this script.
with open(os.environ.get("PORT_FILE", "port.txt"), "w") as f:
    f.write(name)
print(f"fake nano on {name} (boot_delay={boot_delay}s answer={answer})", flush=True)

t0 = time.time()
buf = b""
deadline = time.time() + 12
while time.time() < deadline:
    r, _, _ = select.select([master], [], [], 0.1)
    if not r:
        continue
    try:
        data = os.read(master, 256)
    except OSError:
        break
    if not data:
        break
    buf += data
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        line = line.strip()
        if line == b"PING":
            if time.time() - t0 < boot_delay:
                print(f"  [{time.time()-t0:.2f}s] PING dropped (still booting)", flush=True)
                continue
            if not answer:
                print(f"  [{time.time()-t0:.2f}s] PING ignored (no sketch)", flush=True)
                continue
            reply = b"PONG nanov3 %d\n" % int((time.time() - t0) * 1000)
            os.write(master, reply)
            print(f"  [{time.time()-t0:.2f}s] PING -> {reply!r}", flush=True)
            deadline = time.time() + 1
print("fake nano exiting", flush=True)
