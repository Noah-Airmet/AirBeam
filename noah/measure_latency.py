# /// script
# dependencies = ["numpy", "scipy"]
# ///
"""Measure output latency of HomePod (AirBeam) vs the Mac's built-in speaker.

Plays a short chirp on one device at a time while recording a USB mic, finds the
chirp in the recording by cross-correlation, and compares devices. Both paths
share the same sox start-up overhead, so the difference is the extra latency.
usage: uv run measure_latency.py ["HomePod device"] [ref device] [mic] [runs]
"""
import subprocess, sys, time, tempfile, os
import numpy as np
from scipy.io import wavfile
from scipy.signal import chirp, correlate

hp = sys.argv[1] if len(sys.argv) > 1 else "Kitchen (HomePod)"
ref = sys.argv[2] if len(sys.argv) > 2 else "Mac mini Speakers"
mic = sys.argv[3] if len(sys.argv) > 3 else "Samson Q2U Microphone"
runs = int(sys.argv[4]) if len(sys.argv) > 4 else 3
SR, LEAD = 48000, 1.0
tmp = tempfile.mkdtemp()
t = np.arange(int(0.15 * SR)) / SR
sweep = chirp(t, 300, t[-1], 6000) * np.hanning(len(t))
sig = np.concatenate([np.zeros(int(LEAD * SR)), 0.7 * sweep, np.zeros(int(1.0 * SR))])
play = os.path.join(tmp, "chirp.wav")
wavfile.write(play, SR, (sig * 32767).astype(np.int16))

def once(dev, i):
    rec = os.path.join(tmp, f"rec{i}.wav")
    r = subprocess.Popen(["sox", "-q", "-t", "coreaudio", mic, "-r", str(SR), "-c", "1", rec, "trim", "0", "5.5"])
    time.sleep(0.8)
    # Play via the system default output: sox silently falls back to the
    # default when a named device doesn't match, which fooled the first runs.
    subprocess.run(["sox", "-q", play, "-d"])
    r.wait()
    _, x = wavfile.read(rec)
    x = x.astype(np.float64); x -= x.mean()
    c = np.abs(correlate(x, sweep, mode="valid"))
    k = int(np.argmax(c))
    return k / SR, c[k] / (np.median(c) + 1e-9)

SAS = "/opt/homebrew/bin/SwitchAudioSource"
orig = subprocess.run([SAS, "-c", "-t", "output"], capture_output=True, text=True).stdout.strip()
out = {}
for name, dev in (("built-in", ref), ("HomePod", hp)):
    subprocess.run([SAS, "-s", dev, "-t", "output"], capture_output=True, check=True)
    time.sleep(1.0)
    vals = []
    for i in range(runs):
        at, q = once(dev, f"{name}{i}")
        print(f"{name:9s} run {i+1}: chirp heard at {at*1000:7.0f} ms (peak/median {q:.0f})")
        if q > 8: vals.append(at)
        time.sleep(0.4)
    out[name] = np.median(vals) if vals else float("nan")
subprocess.run([SAS, "-s", orig, "-t", "output"], capture_output=True)
print(f"\nHomePod extra latency vs built-in speaker: {(out['HomePod'] - out['built-in'])*1000:.0f} ms")
