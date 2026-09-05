"""Opt-in ALSA/TTY integration test. Requires an idle audio device.

Records the physical input briefly; never runs the loopback pulse calibrator.
All WAVs and logs go into a fresh temporary directory under the build directory.
"""
import os
import pathlib
import pty
import re
import select
import subprocess
import sys
import tempfile
import time

binary = pathlib.Path(sys.argv[1]).resolve()
work = pathlib.Path(tempfile.mkdtemp(prefix="hardware-smoke-", dir=binary.parent))
master, slave = pty.openpty()
process = subprocess.Popen([str(binary), "--monitor", "analog", "--max-seconds", "60"],
                           stdin=slave, stdout=slave, stderr=slave, cwd=work)
os.close(slave)
output = bytearray()

def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if select.select([master], [], [], min(.1, max(0, end-time.monotonic())))[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            output.extend(chunk)

def key(value, seconds):
    if process.poll() is not None:
        raise RuntimeError("Audio process exited early")
    os.write(master, value.encode())
    pump(seconds)

try:
    pump(2)
    rss = pathlib.Path(f"/proc/{process.pid}/status").read_text()
    print(next(line for line in rss.splitlines() if line.startswith("VmRSS:")))
    key(" ", 2)   # base
    key(" ", 2)   # play
    key(" ", 3)   # overdub, >1 lap
    key(" ", 2)   # commit
    key("u", 1)
    key("u", 1)
    key("r", 1)
    key("r", 1)
    key("s", 1)
    key("w", 4)
    assert len(list((work / "recordings").glob("*.wav"))) == 1, "WAV not saved"
    key("l", 3)
    key(" ", 2)
    key("f", 5)
    key("q", 1)
    assert process.wait(timeout=5) == 0
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)
    os.close(master)
    (work / "console.log").write_bytes(output)

text = output.decode(errors="replace")
assert "SCHED_FIFO 80" in text, "RT priority was not acquired"
assert "Saved:" in text and "Loaded:" in text, "File round trip failed"
assert "FATAL" not in text and "[ERROR]" not in text
counts = [(int(a), int(b)) for a, b in re.findall(r"XRUN:C:(\d+)/P:(\d+)", text)]
assert counts, "No audio telemetry"
print("XRUN maxima:", max(a for a,b in counts), max(b for a,b in counts))
assert all(a == b == 0 for a,b in counts), "Audio underrun/overrun"
print("ALSA record/play/overdub/undo/reverse/save/load/fade/quit: PASS")
print("Artifacts:", work)
