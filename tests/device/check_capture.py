#!/usr/bin/env python3
"""Check what NAM Player does on the MPC itself, against the NAM reference engine.

    python3 tests/device/check_capture.py --profile "~/Downloads/.../My Profile.nam" --record 8
    python3 tests/device/check_capture.py --profile "....nam" --capture tests/device/capture-1.wav

With --record N the script asks the plugin running on the MPC (over the live link,
http://mpc-sample.local:9626) to record N seconds, and downloads the recording. Start
whatever you want to test on the MPC first. Nothing about the MPC's sound changes.

The recording has four channels, taken inside the plugin:
    1  what the plugin received (left)        3  what the model put out (64 samples later)
    2  what went into the model (after Drive) 4  what the plugin put out (left)

The script then feeds channel 2 to NeuralAmpModelerCore on this computer, with the same
profile, and compares the result with channel 3. A figure below about -60 dB means the
MPC produced the same audio as the reference. It also writes two files to listen to.
If --file names the WAV being played on the MPC (default: tests/device/NAM TEST.wav, if
you have put one there), it also reports how the MPC delivered that file to the plugin
(level, polarity, damage).

Needs numpy, scipy and a native build (python3 build.py --target native).
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
import urllib.request

import numpy as np
from scipy import signal
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import run_tests as T   # noqa: E402

SR = T.SR


def db(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-12)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--profile", required=True, help="the .nam file on this computer that is loaded on the MPC")
    ap.add_argument("--record", type=float, metavar="SECONDS", help="record on the MPC now (0.5 to 8 s)")
    ap.add_argument("--capture", help="use a recording made earlier instead")
    ap.add_argument("--file", default=os.path.join(HERE, "NAM TEST.wav"), help="the file being played on the MPC, if known")
    ap.add_argument("--mpc", default="http://192.168.155.1:9626", help="address of the plugin's live link")
    args = ap.parse_args()
    profile = os.path.expanduser(args.profile)
    lite = False

    if args.record:
        status = json.loads(urllib.request.urlopen(args.mpc + "/api/status", timeout=5).read())
        cur, eng, par = status["current"], status["engine"], status["params"]
        lite = bool(eng["lite"])
        print("on the MPC: %s / %s | %s%s, model at %s Hz | MPC %s Hz, %s-frame blocks" % (
            cur["folder"], cur["file"], eng["kind"], " (light version)" if lite else "", eng["rate"], status["rate"], status["block"]))
        print("controls: " + ", ".join("%s %s" % (k, v["text"]) for k, v in par.items()))
        req = urllib.request.Request(args.mpc + "/api/capture?seconds=%g" % args.record, method="POST", headers={"X-NAM-Player": "1"})
        urllib.request.urlopen(req, timeout=5).read()
        time.sleep(args.record + 1.0)
        raw = urllib.request.urlopen(args.mpc + "/api/capture.wav", timeout=30).read()
        after = json.loads(urllib.request.urlopen(args.mpc + "/api/status", timeout=5).read())
        print("CPU: %.0f %% average, %.0f %% peak, %d late blocks" % (after["cpu"] * 100, after["cpuPeak"] * 100, after["overruns"]))
        args.capture = os.path.join(HERE, time.strftime("capture-%Y%m%d-%H%M%S.wav"))
        open(args.capture, "wb").write(raw)
        print("saved " + args.capture)
    elif not args.capture:
        ap.error("give --record SECONDS or --capture FILE")

    cap = np.frombuffer(open(args.capture, "rb").read()[44:], dtype=np.float32).reshape(-1, 4).astype(np.float64)
    received, into_model, model_out, out = cap[:, 0], cap[:, 1], cap[:, 2], cap[:, 3]
    print("levels (dBFS rms): received %.1f, into the model %.1f, model output %.1f, plugin output %.1f" % (
        db(received), db(into_model), db(model_out), db(out)))
    if db(into_model) < -70:
        sys.exit("nothing was playing through the plugin during the recording")

    start = 0
    if args.file and os.path.isfile(args.file):
        _, f = wavfile.read(args.file)
        f = f.astype(np.float64) / (32768.0 if f.dtype == np.int16 else 1.0)
        if f.ndim > 1:
            f = f[:, 0]
        c = signal.correlate(received, f, mode="full", method="fft")
        lag = int(np.argmax(np.abs(c))) - (len(f) - 1)
        a0, a1 = max(0, lag), min(len(received), lag + len(f))
        if a1 - a0 > SR:
            seg_c, seg_f = received[a0:a1], f[a0 - lag:a1 - lag]
            g = np.dot(seg_c, seg_f) / np.dot(seg_f, seg_f)
            print("the file as the MPC delivered it to the plugin: %.1f s of it, gain %+.2f dB, %s, difference after gain %.1f dB" % (
                (a1 - a0) / SR, 20 * np.log10(abs(g) + 1e-12), "inverted" if g < 0 else "same polarity", T.esr_db(seg_c, g * seg_f)))
            start = a0

    tmp = tempfile.mkdtemp(prefix="namplayer-device-")
    try:
        _, core = T.build_tools(tmp)
        seg = into_model[start:][: 6 * SR]
        w = T.dc_block(model_out[start:][: 6 * SR])        # the plugin applies this filter next; the reference does too
        results = []
        for k in range(147):   # where the plugin's 48 kHz sample instants fall within this recording is not known: try each
            saved, T.T0 = T.T0, T.T0 + k
            ref, loud = T.reference(core, tmp, profile, 0.0 if lite else 1.0, seg)
            T.T0 = saved
            a, b = T.band(w)[T.LAT + SR // 2:], T.band(ref)[SR // 2: len(w) - T.LAT]
            results.append((T.esr_db(a, b), ref, loud))
        results.sort(key=lambda r: r[0])
        e, ref, loud = results[0]
        print("model output on the MPC vs NAM Core fed the identical input: %.1f dB  (%s)" % (
            e, "the same audio" if e < -60 else "DIFFERENT: the MPC is not producing what the reference does"))
        gain = 10 ** ((-18.0 - loud) / 20) if loud is not None else 1.0
        dev = out[start + T.LAT:][: len(ref)]
        base = os.path.splitext(args.capture)[0]
        wavfile.write(base + " - from the MPC.wav", SR, dev.astype(np.float32))
        wavfile.write(base + " - reference engine.wav", SR, (ref * gain)[: len(dev)].astype(np.float32))
        print("to listen: \"%s - from the MPC.wav\" and \"... - reference engine.wav\" (the reference assumes Normalize on, Tone off, Mix 100, Level 0)" % os.path.basename(base))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
