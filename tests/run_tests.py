#!/usr/bin/env python3
"""Regression tests for NAM Player. Needs numpy and scipy, and a native build:

    python3 build.py --target native
    python3 tests/run_tests.py

What is checked, against references that share no code with the plugin's signal path:
  * the audio: plugin output vs. scipy resampling + NeuralAmpModelerCore, for A2-Full and
    A2-Lite (fast engines), A1 standard, an LSTM and two slimmable models (NAM Core path)
  * latency: the wet path is exactly 64 samples behind, with and without resampling,
    and Mix blends dry and wet in phase
  * block-size independence: random 1..512-frame blocks give the same audio as 128-frame blocks
  * profile switching while audio runs, including a corrupt file
  * state: a saved project finds its profile again after the card's contents change
  * the CPU guard: Auto drops an A2 profile to A2-Lite; a model that cannot be slimmed is bypassed
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from scipy import signal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "plugins", "NAM Player")
SDK = os.environ.get("MPC_SDK", os.path.join(ROOT, "sdk"))
LIB = os.path.join(PROJECT, "build", "native", "nam-player" + (".dylib" if sys.platform == "darwin" else ".so"))
OBJ = os.path.join(PROJECT, "build", "obj", "native")
THIRD = os.path.join(PROJECT, "third_party")
FIX = os.path.join(ROOT, "tests", "fixtures")
ZIG = [os.path.join(SDK, ".sdk-tools", "zigenv", "bin", "python"), "-m", "ziglang", "c++"]
SR = 44100
LAT = 64

# Normalised parameter values.
LEVEL0, DRIVE0, TONE_OFF = 24.0 / 36.0, 0.5, 1.0
Q_AUTO, Q_LITE, Q_FULL = 0.0, 0.5, 1.0

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  (" + detail + ")") if detail else ""))


def esr_db(a, b):
    return 10 * np.log10(np.sum((a - b) ** 2) / (np.sum(b ** 2) + 1e-30) + 1e-30)


def dc_block(x):
    r = 1.0 - 2.0 * np.pi * 10.0 / SR
    return signal.lfilter([1.0, -1.0], [1.0, -r], x)


def build_tools(tmp):
    host = os.path.join(tmp, "host")
    core = os.path.join(tmp, "core_render")
    subprocess.check_call(ZIG + ["-std=c++17", "-O2", "-I", os.path.join(SDK, "framework"),
                                 os.path.join(ROOT, "tests", "host.cpp"), "-o", host])
    objs = [os.path.join(OBJ, f) for f in os.listdir(OBJ) if f.startswith("NeuralAmpModelerCore") and f.endswith(".o")]
    subprocess.check_call(ZIG + ["-std=c++20", "-O2", "-w", "-DNAM_SAMPLE_FLOAT", "-DNDEBUG",
                                 "-isystem", os.path.join(THIRD, "eigen"), "-isystem", os.path.join(THIRD, "nlohmann"),
                                 "-isystem", os.path.join(THIRD, "NeuralAmpModelerCore"),
                                 "-isystem", os.path.join(THIRD, "NeuralAmpModelerCore", "NAM"),
                                 os.path.join(ROOT, "tests", "core_render.cpp")] + objs + ["-o", core])
    return host, core


def linear_identity(path, rate):
    json.dump({"version": "0.5.4", "architecture": "Linear", "config": {"receptive_field": 1, "bias": False},
               "weights": [1.0], "sample_rate": rate}, open(path, "w"))


def make_library(tmp, name="NAM Profiles", extra=False):
    lib = os.path.join(tmp, name)
    amps, util, more = os.path.join(lib, "Amps"), os.path.join(lib, "Util"), os.path.join(lib, "More")
    os.makedirs(amps), os.makedirs(util), os.makedirs(more)
    models = os.path.join(PROJECT, "Content", "Models")
    shutil.copy(os.path.join(models, "Demo Classic A1.nam"), os.path.join(amps, "A1 Standard Demo.nam"))
    shutil.copy(os.path.join(models, "Demo A2.nam"), os.path.join(amps, "A2 Demo.nam"))
    for f in ("lstm.nam", "slimmable_container.nam", "slimmable_wavenet.nam", "wavenet.nam"):
        shutil.copy(os.path.join(FIX, f), more)
    linear_identity(os.path.join(util, "identity48.nam"), 48000)
    linear_identity(os.path.join(util, "identity441.nam"), 44100)
    open(os.path.join(util, "broken.nam"), "w").write("{ this is not a model")
    open(os.path.join(util, "._identity48.nam"), "w").write("macOS resource fork junk")   # must be ignored
    if extra:   # a changed card: new folders and files before and between the old ones
        os.makedirs(os.path.join(lib, "AAA New"))
        linear_identity(os.path.join(lib, "AAA New", "x.nam"), 48000)
        linear_identity(os.path.join(amps, "A0 first.nam"), 48000)
        linear_identity(os.path.join(util, "aaa.nam"), 48000)
    return lib


# Folder order is alphabetical: Amps, More, Util. A knob position in the middle of slot i of n:
def slot(i, n):
    return (i + 0.5) / n


FOLDER = {"Amps": slot(0, 3), "More": slot(1, 3), "Util": slot(2, 3)}
AMPS = {"A1": slot(0, 2), "A2": slot(1, 2)}
MORE = {"lstm": slot(0, 4), "slim_container": slot(1, 4), "slim_wavenet": slot(2, 4), "wavenet": slot(3, 4)}
UTIL = {"broken": slot(0, 3), "identity441": slot(1, 3), "identity48": slot(2, 3)}


class Runner:
    def __init__(self, tmp, host, lib_dir):
        self.tmp, self.host, self.lib_dir = tmp, host, lib_dir
        self.n = 0

    def run(self, stereo, commands, env=None):
        self.n += 1
        inp = os.path.join(self.tmp, "in%d.f32" % self.n)
        out = os.path.join(self.tmp, "out%d.f32" % self.n)
        stereo.astype(np.float32).tofile(inp)
        e = dict(os.environ)
        e.update({"NAM_PLAYER_SYNC": "1", "NAM_PLAYER_PROFILES": self.lib_dir,
                  "MPC_PLUGIN_FOLDER": os.path.join(self.tmp, "no-factory")})
        e.update(env or {})
        p = subprocess.run([self.host, LIB, inp, out] + commands, env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            raise RuntimeError("host failed (%d): %s" % (p.returncode, p.stderr.decode()))
        y = np.fromfile(out, dtype=np.float32).reshape(-1, 2).astype(np.float64)
        return y, p.stdout.decode(), p.stderr.decode()


def base(folder, model, quality=Q_FULL, mix=1.0, normalize=0.0, drive=DRIVE0, tone=TONE_OFF):
    return ["p0=%f" % LEVEL0, "p1=%f" % drive, "p2=%f" % mix, "p3=%f" % tone, "p4=%f" % quality,
            "p5=%f" % normalize, "p6=%f" % model, "p7=%f" % folder]


def guitar(seconds, seed=1):
    rng = np.random.default_rng(seed)
    n = int(seconds * SR)
    t = np.arange(n) / SR
    x = np.zeros(n)
    for start in np.arange(0.05, seconds - 0.3, 0.37):
        f0 = rng.choice([82.4, 110.0, 146.8, 196.0, 246.9, 329.6])
        i = int(start * SR)
        tt = t[: n - i]
        note = sum(np.sin(2 * np.pi * f0 * h * tt + rng.uniform(0, 6.28)) / h * np.exp(-tt * (2.0 + 0.8 * h))
                   for h in range(1, 12))
        x[i:] += note * rng.uniform(0.3, 1.0)
    x += 0.002 * rng.standard_normal(n)
    x *= 0.5 / np.max(np.abs(x))
    left = x
    right = 0.7 * np.roll(x, 5) + 0.01 * rng.standard_normal(n)   # a different right channel
    return np.stack([left, right], axis=1)


_fir = {}


def resample(x, up, down):
    """A near-ideal polyphase resampler (far longer than the plugin's), zero delay."""
    key = max(up, down)
    if key not in _fir:
        _fir[key] = signal.firwin(key * 256 + 1, 0.98 / key, window=("kaiser", 12.0))
    return signal.resample_poly(x, up, down, window=_fir[key])


def band(x):
    """Keep what both resamplers pass cleanly, so the comparison is about the model."""
    return signal.sosfiltfilt(signal.butter(8, 15000, fs=SR, output="sos"), x)


UP_DELAY = 32   # host samples of delay in the plugin's first converter (half its filter length)
T0 = 256        # host sample at which the plugin's first model starts: after the 256-sample start-up fade


def reference(core, tmp, model_path, slim, mono, rate=48000):
    """Reference resampler -> NAM Core -> reference resampler -> DC blocker. Zero delay.

    A model is a nonlinear discrete-time system: what it does to a signal depends, a
    little, on where its sample instants fall (its distortion aliases differently;
    two renders of the A1 demo profile that differ only in that agree to about -30 dB).
    So the reference puts the model's 48 kHz sample instants where the plugin puts
    them: the plugin starts its converters T0 host samples in, and its first converter
    delays the signal by UP_DELAY host samples."""
    n = len(mono)
    start = T0 - UP_DELAY
    x = mono[start:]
    up = resample(x, rate // 300, SR // 300) if rate != SR else x
    a, b = os.path.join(tmp, "ref_in.f32"), os.path.join(tmp, "ref_out.f32")
    up.astype(np.float32).tofile(a)
    out = subprocess.check_output([core, model_path, str(slim), a, b]).decode()
    y = np.fromfile(b, dtype=np.float32).astype(np.float64)
    down = resample(y, SR // 300, rate // 300) if rate != SR else y
    loud = out.split()
    loudness = float(loud[2]) if loud[1] == "yes" else None
    return dc_block(np.concatenate([np.zeros(start), down])[:n]), loudness


def aligned(y_wet, ref, skip=0.5):
    """Plugin output delayed by LAT against a zero-delay reference, skipping the start-up fade."""
    a, b = y_wet[LAT:], ref[: len(y_wet) - LAT]
    s = int(skip * SR)
    return a[s:-2000], b[s:-2000]


def model_esr(y_wet, ref, skip=0.5):
    a, b = aligned(band(y_wet), band(ref), skip)
    return esr_db(a, b), 20 * np.log10(np.sqrt(np.mean(b ** 2)) + 1e-12)


def main():
    if not os.path.isfile(LIB):
        sys.exit("native build not found: run  python3 build.py --target native  first")
    tmp = tempfile.mkdtemp(prefix="namplayer-test-")
    host, core = build_tools(tmp)
    lib_dir = make_library(tmp)
    r = Runner(tmp, host, lib_dir)
    stereo = signal.sosfiltfilt(signal.butter(8, 16000, fs=SR, output="sos"), guitar(3.0), axis=0)
    mono = 0.5 * (stereo[:, 0] + stereo[:, 1])
    n = len(mono)
    run = ["run=%d" % n]
    amps, more, util = [os.path.join(lib_dir, d) for d in ("Amps", "More", "Util")]

    # ---- library and value text ------------------------------------------------------------
    _, text, _ = r.run(stereo[:256], base(FOLDER["Util"], UTIL["identity48"]) + ["text=7", "text=6", "run=256"])
    check("library: folder and model names, macOS '._' files ignored", "text 7: Util" in text and "text 6: 3 identity48" in text, text.strip().replace("\n", " | "))

    # ---- latency and mixing ------------------------------------------------------------------
    y, _, _ = r.run(stereo, base(FOLDER["Util"], UTIL["identity441"]) + run)
    a, b = aligned(y[:, 0], dc_block(mono))
    e = esr_db(a, b)
    check("latency: 44.1 kHz model (no resampling) is exactly 64 samples behind", e < -100, "ESR %.1f dB" % e)
    check("wet signal is mono (identical on both sides)", np.max(np.abs(y[:, 0] - y[:, 1])) == 0.0)

    lp = signal.sosfiltfilt(signal.butter(8, 12000, fs=SR, output="sos"), stereo, axis=0)   # stay inside the converters' passband
    lp_mono = 0.5 * (lp[:, 0] + lp[:, 1])
    y, _, _ = r.run(lp, base(FOLDER["Util"], UTIL["identity48"]) + run)
    a, b = aligned(y[:, 0], dc_block(lp_mono))
    e = esr_db(a, b)
    check("latency: 48 kHz model (through both converters) is exactly 64 samples behind", e < -60, "ESR %.1f dB" % e)

    y, _, _ = r.run(lp, base(FOLDER["Util"], UTIL["identity48"], mix=0.5) + run)
    for ch, name in ((0, "left"), (1, "right")):
        a, b = aligned(y[:, ch], 0.5 * lp[:, ch] + 0.5 * dc_block(lp_mono))
        e = esr_db(a, b)
        check("mix 50%%: dry (%s, stereo kept) and wet add in phase" % name, e < -60, "ESR %.1f dB" % e)

    y, _, _ = r.run(lp, base(FOLDER["Util"], UTIL["identity48"], mix=0.0) + run)
    a, b = aligned(y[:, 1], lp[:, 1])
    check("mix 0%: the dry signal alone, delayed 64 samples", esr_db(a, b) < -110, "ESR %.1f dB" % esr_db(a, b))

    y, _, _ = r.run(lp, base(FOLDER["Util"], UTIL["identity48"], drive=0.75) + run)   # +12 dB
    a, b = aligned(y[:, 0], dc_block(lp_mono))
    gain = 20 * np.log10(np.sqrt(np.mean(a ** 2) / np.mean(b ** 2)))
    check("drive +12 dB", abs(gain - 12.0) < 0.05, "%.2f dB" % gain)

    tone_1k = np.log(1000.0 / 500.0) / np.log(20000.0 / 500.0)
    y, _, _ = r.run(lp, base(FOLDER["Util"], UTIL["identity48"], tone=tone_1k) + run)
    f, pxx = signal.welch(y[SR // 2:, 0], SR, nperseg=4096)
    f, pref = signal.welch(dc_block(lp_mono)[SR // 2:], SR, nperseg=4096)
    def band(p, lo, hi): return 10 * np.log10(np.sum(p[(f >= lo) & (f < hi)]))
    lo_change = band(pxx, 100, 400) - band(pref, 100, 400)
    hi_change = band(pxx, 3800, 4200) - band(pref, 3800, 4200)
    check("tone at 1 kHz: lows kept, 4 kHz down about 24 dB", abs(lo_change) < 1.0 and -27 < hi_change < -21,
          "lows %+.1f dB, 4 kHz %+.1f dB" % (lo_change, hi_change))

    # ---- the models, against NAM Core ------------------------------------------------------------
    cases = [
        ("A2-Full (fast engine)", FOLDER["Amps"], AMPS["A2"], Q_FULL, os.path.join(amps, "A2 Demo.nam"), 1.0, -45),
        ("A2-Lite (fast engine)", FOLDER["Amps"], AMPS["A2"], Q_LITE, os.path.join(amps, "A2 Demo.nam"), 0.0, -45),
        ("A1 standard WaveNet (NAM Core)", FOLDER["Amps"], AMPS["A1"], Q_FULL, os.path.join(amps, "A1 Standard Demo.nam"), 1.0, -45),
        ("LSTM (NAM Core)", FOLDER["More"], MORE["lstm"], Q_FULL, os.path.join(more, "lstm.nam"), 1.0, -45),
        ("slimmable container, full (NAM Core)", FOLDER["More"], MORE["slim_container"], Q_FULL, os.path.join(more, "slimmable_container.nam"), 1.0, -45),
        ("slimmable container, lite (NAM Core)", FOLDER["More"], MORE["slim_container"], Q_LITE, os.path.join(more, "slimmable_container.nam"), 0.0, -45),
        ("slimmable WaveNet, full (NAM Core)", FOLDER["More"], MORE["slim_wavenet"], Q_FULL, os.path.join(more, "slimmable_wavenet.nam"), 1.0, -45),
        ("slimmable WaveNet, lite (NAM Core)", FOLDER["More"], MORE["slim_wavenet"], Q_LITE, os.path.join(more, "slimmable_wavenet.nam"), 0.0, -45),
        ("small WaveNet (NAM Core)", FOLDER["More"], MORE["wavenet"], Q_FULL, os.path.join(more, "wavenet.nam"), 1.0, -45),
    ]
    refs = {}
    for name, folder, model, quality, path, slim, limit in cases:
        ref, loudness = reference(core, tmp, path, slim, mono)
        refs[name] = (ref, loudness)
        y, _, err = r.run(stereo, base(folder, model, quality) + run)
        e, rms = model_esr(y[:, 0], ref)
        check("audio matches the NAM reference: " + name, e < limit and rms > -80,
              "ESR %.1f dB, rms %.1f dBFS%s" % (e, rms, (" " + err.strip()) if err.strip() else ""))

    # ---- loudness normalisation ---------------------------------------------------------------------
    ref, loudness = refs["A2-Full (fast engine)"]
    y, _, _ = r.run(stereo, base(FOLDER["Amps"], AMPS["A2"], Q_FULL, normalize=1.0) + run)
    a, b = aligned(y[:, 0], ref)
    gain = 20 * np.log10(np.sqrt(np.mean(a ** 2) / np.mean(b ** 2)))
    check("normalize: A2 profile brought to -18 dB loudness", loudness is not None and abs(gain - (-18.0 - loudness)) < 0.1,
          "gain %+.2f dB, file loudness %s" % (gain, loudness))

    # ---- block-size independence ----------------------------------------------------------------------
    for name, folder, model, quality in (("A2-Full", FOLDER["Amps"], AMPS["A2"], Q_FULL), ("A1 standard", FOLDER["Amps"], AMPS["A1"], Q_FULL)):
        y1, _, _ = r.run(stereo, base(folder, model, quality) + ["block=128"] + run)
        y2, _, _ = r.run(stereo, base(folder, model, quality) + ["block=0"] + run)
        y3, _, _ = r.run(stereo, base(folder, model, quality) + ["block=32"] + run)
        s = SR // 2
        e2, e3 = esr_db(y2[s:, 0], y1[s:, 0]), esr_db(y3[s:, 0], y1[s:, 0])
        check("block-size independent: %s, random 1..512 and 32 vs 128" % name, e2 < -90 and e3 < -90, "ESR %.1f / %.1f dB" % (e2, e3))

    # ---- switching while running ------------------------------------------------------------------------
    cmds = base(FOLDER["Amps"], AMPS["A2"], Q_AUTO, normalize=1.0) + ["block=0"]
    picks = [(FOLDER["Amps"], AMPS["A1"]), (FOLDER["Util"], UTIL["broken"]), (FOLDER["More"], MORE["lstm"]),
             (FOLDER["Amps"], AMPS["A2"]), (FOLDER["Util"], UTIL["identity48"]), (FOLDER["More"], MORE["slim_container"]),
             (FOLDER["Util"], UTIL["identity441"]), (FOLDER["Amps"], AMPS["A2"])]
    step = n // (len(picks) + 1)
    cmds += ["run=%d" % step]
    for folder, model in picks:
        cmds += ["p7=%f" % folder, "p6=%f" % model, "run=%d" % step]
    y, _, _ = r.run(stereo, cmds)
    jump = np.max(np.abs(np.diff(y[:, 0])))
    check("switching 8 profiles mid-stream with random blocks: finite and bounded", np.all(np.isfinite(y)) and np.max(np.abs(y)) < 4.0,
          "peak %.2f" % np.max(np.abs(y)))

    # Click test: swap between two models that both pass the signal unchanged. A 100 Hz sine
    # never moves more than 0.0071 per sample; the fades may add 0.5 / 256 = 0.002 to that.
    t = np.arange(2 * SR) / SR
    sine = np.repeat((0.5 * np.sin(2 * np.pi * 100 * t))[:, None], 2, axis=1)
    cmds = base(FOLDER["Util"], UTIL["identity48"]) + ["block=0", "run=20000"]
    for i in range(6):
        cmds += ["p6=%f" % (UTIL["identity441"] if i % 2 == 0 else UTIL["identity48"]), "run=10000"]
    y, _, _ = r.run(sine, cmds)
    jump = np.max(np.abs(np.diff(y[2000:, 0])))
    dips = int(np.sum(np.diff((np.abs(signal.hilbert(y[2000:-2000, 0])) < 0.05).astype(int)) == 1))
    check("switching is click-free: the wet signal fades out and in around each of 6 swaps", jump < 0.011 and dips == 6,
          "largest sample-to-sample step %.4f, %d fades" % (jump, dips))

    # ---- a corrupt file ------------------------------------------------------------------------------------
    y, text, err = r.run(lp, base(FOLDER["Util"], UTIL["broken"]) + run + ["text=6"])
    a, b = aligned(y[:, 0], dc_block(lp_mono))
    check("corrupt .nam: audio passes through, knob shows ERR", esr_db(a, b) < -100 and "text 6: ERR 1 broken" in text, text.strip())

    # ---- state ------------------------------------------------------------------------------------------------
    state = os.path.join(tmp, "state.bin")
    _, text, _ = r.run(stereo[:512], base(FOLDER["Amps"], AMPS["A2"]) + ["run=256", "save", "p6=0.1", "p7=0.9", "text=6", "load", "text=7", "text=6", "run=256", "save=" + state])
    check("state: save, move the knobs, load: the profile comes back", text.split("\n")[1:3] == ["text 7: Amps", "text 6: 2 A2 Demo"], text.strip().replace("\n", " | "))

    lib2 = make_library(tmp, "NAM Profiles changed", extra=True)
    r2 = Runner(tmp, host, lib2)
    y, text, _ = r2.run(stereo, ["load=" + state, "text=7", "text=6"] + run + ["text=6"])
    ref, _ = refs["A2-Full (fast engine)"]
    e, _ = model_esr(y[:, 0], ref)
    ok_text = "text 7: Amps" in text and text.count("text 6: 3 A2 Demo") == 2
    check("state: after profiles were added to the card, the project still loads its own profile", ok_text and e < -45,
          "%s, ESR %.1f dB" % (text.strip().replace("\n", " | "), e))
    _, text, _ = r2.run(stereo[:512], ["load=" + state, "run=128", "p6=0.01", "text=7", "text=6"])
    check("state: moving Model afterwards browses inside the restored folder", "text 7: Amps" in text and "text 6: 1 A0 first" in text, text.strip().replace("\n", " | "))
    _, text, _ = r2.run(stereo[:512], ["load=" + state, "run=128", "p7=0.99", "text=7", "text=6"])
    check("state: moving Folder afterwards goes back to knob positions", "text 7: Util" in text, text.strip().replace("\n", " | "))

    # ---- CPU guard ----------------------------------------------------------------------------------------------
    ref_lite, _ = refs["A2-Lite (fast engine)"]
    for fake in ("0.6", "0.95"):
        y, text, _ = r.run(stereo, base(FOLDER["Amps"], AMPS["A2"], Q_AUTO) + run + ["text=6"], env={"NAM_PLAYER_TEST_LOAD": fake})
        # The swap happens at a moment the test does not control, so the sample instants differ from the
        # reference's (see reference()): tell the two versions apart rather than matching one tightly.
        e, _ = model_esr(y[:, 0], ref_lite, skip=1.5)
        e_full, _ = model_esr(y[:, 0], refs["A2-Full (fast engine)"][0], skip=1.5)
        check("CPU guard: Auto drops an overloaded A2 profile to A2-Lite (load %s)" % fake, e < -25 and e < e_full - 15 and "CPU!" not in text,
              "ESR vs Lite %.1f dB, vs Full %.1f dB, %s" % (e, e_full, text.strip()))
    y, text, _ = r.run(stereo, base(FOLDER["Amps"], AMPS["A2"], Q_FULL) + run + ["text=6"], env={"NAM_PLAYER_TEST_LOAD": "0.6"})
    e, _ = model_esr(y[:, 0], refs["A2-Full (fast engine)"][0], skip=1.5)
    check("CPU guard: Quality = Full is respected below the bypass threshold", e < -45, "ESR vs Full %.1f dB" % e)
    y, text, _ = r.run(lp, base(FOLDER["Amps"], AMPS["A1"], Q_AUTO) + run + ["text=6"], env={"NAM_PLAYER_TEST_LOAD": "0.95"})
    a, b = aligned(y[:, 0], dc_block(lp_mono), skip=1.5)
    check("CPU guard: a model that cannot be slimmed is bypassed and marked CPU!", esr_db(a, b) < -60 and "text 6: CPU! 1 A1 Standard Demo" in text,
          "ESR vs dry %.1f dB, %s" % (esr_db(a, b), text.strip()))
    y, text, _ = r.run(stereo, base(FOLDER["Amps"], AMPS["A1"], Q_AUTO) + run + ["text=6"], env={"NAM_PLAYER_TEST_LOAD": "0.6"})
    e, _ = model_esr(y[:, 0], refs["A1 standard WaveNet (NAM Core)"][0], skip=1.5)
    check("CPU guard: a heavy but playable model keeps running", e < -45 and "CPU!" not in text, "ESR %.1f dB" % e)

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
