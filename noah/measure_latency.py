# /// script
# dependencies = ["numpy", "scipy"]
# ///
"""Measure HomePod (AirBeam) latency vs the Mac's built-in speaker with a USB mic.

Plays a 1 kHz beep on the reference speaker and a 3 kHz beep on the HomePod at
the same instant, records the mic, and reports how much later the 3 kHz beep lands.
usage: uv run measure_latency.py "<homepod device>" [ref device] [mic device] [runs]
"""
import subprocess, sys, time, tempfile, os
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfiltfilt

hp = sys.argv[1]
ref = sys.argv[2] if len(sys.argv) > 2 else "Mac mini Speakers"
mic = sys.argv[3] if len(sys.argv) > 3 else "Samson Q2U Microphone"
runs = int(sys.argv[4]) if len(sys.argv) > 4 else 3
SR = 48000
tmp = tempfile.mkdtemp()

def tone(freq, path):
    t = np.arange(int(0.04 * SR)) / SR
    beep = 0.8 * np.sin(2 * np.pi * freq * t) * np.hanning(len(t))
    sig = np.concatenate([np.zeros(int(1.5 * SR)), beep, np.zeros(int(1.0 * SR))])
    wavfile.write(path, SR, (sig * 32767).astype(np.int16))

def onset(x, freq):
    sos = butter(4, [freq * 0.8, freq * 1.2], btype="band", fs=SR, output="sos")
    env = np.abs(sosfiltfilt(sos, x))
    k = int(0.005 * SR)
    env = np.convolve(env, np.ones(k) / k, mode="same")
    noise = np.median(env[: int(0.5 * SR)]) + 1e-9
    idx = np.argmax(env > max(env.max() * 0.3, noise * 8))
    return idx / SR, env.max() / noise

a, b = os.path.join(tmp, "a.wav"), os.path.join(tmp, "b.wav")
tone(1000, a); tone(3000, b)
results = []
for r in range(runs):
    rec = os.path.join(tmp, f"rec{r}.wav")
    recp = subprocess.Popen(["sox", "-q", "-t", "coreaudio", mic, "-r", str(SR), "-c", "1", rec, "trim", "0", "7"])
    time.sleep(1.0)
    p1 = subprocess.Popen(["sox", "-q", a, "-t", "coreaudio", ref])
    p2 = subprocess.Popen(["sox", "-q", b, "-t", "coreaudio", hp])
    p1.wait(); p2.wait(); recp.wait()
    sr, x = wavfile.read(rec)
    x = x.astype(np.float64)
    ta, sa = onset(x, 1000); tb, sb = onset(x, 3000)
    d = (tb - ta) * 1000
    print(f"run {r+1}: ref beep {ta:.3f}s (snr {sa:.0f}x), homepod beep {tb:.3f}s (snr {sb:.0f}x), delta {d:.0f} ms")
    results.append(d)
    time.sleep(0.5)
print(f"median HomePod lag vs built-in speaker: {np.median(results):.0f} ms")
