#!/usr/bin/env python3
"""Tests for the live link: the web server inside the plugin (src/LinkServer.cpp).

    python3 build.py --target native && python3 tests/test_link.py

The plugin runs in tests/host.cpp at real-time pace, with its real worker and link
threads, while this script talks to it over HTTP the way the page and the companion do:
send a profile and hear it start, audition, remove, and try the things that must be refused.
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run_tests as T   # noqa: E402

PORT = 19626
BASE = "http://127.0.0.1:%d" % PORT
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  (" + str(detail) + ")") if detail else ""))


def http(method, path, body=None, trusted=True):
    req = urllib.request.Request(BASE + path, data=body, method=method, headers={"X-NAM-Player": "1"} if trusted else {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if r.headers.get("Content-Type", "").startswith("application/json") else raw)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, raw


def status():
    return http("GET", "/api/status", trusted=False)[1]


def main():
    tmp = tempfile.mkdtemp(prefix="namlink-test-")
    host = os.path.join(tmp, "host")
    subprocess.check_call(T.ZIG + ["-std=c++17", "-O2", "-I", os.path.join(T.SDK, "framework"), os.path.join(T.ROOT, "tests", "host.cpp"), "-o", host])
    profiles = os.path.join(tmp, "card", "NAM Profiles")
    os.makedirs(os.path.join(profiles, "Start"))
    T.linear_identity(os.path.join(profiles, "Start", "identity.nam"), 44100)   # passes audio through
    plugin_folder = os.path.join(tmp, "plugin")                                  # an empty "factory" with the catalogue
    os.makedirs(os.path.join(plugin_folder, "Content", "Link"))
    shutil.copy(os.path.join(T.PROJECT, "Content", "Link", "community.txt"), os.path.join(plugin_folder, "Content", "Link"))
    a2 = open(os.path.join(T.PROJECT, "Content", "Models", "Demo A2.nam"), "rb").read()
    a1 = open(os.path.join(T.PROJECT, "Content", "Models", "Demo Classic A1.nam"), "rb").read()

    seconds = 9
    t = np.arange(seconds * T.SR) / T.SR
    sine = np.repeat((0.25 * np.sin(2 * np.pi * 220 * t))[:, None], 2, axis=1)
    sine.astype(np.float32).tofile(os.path.join(tmp, "in.f32"))
    env = dict(os.environ, NAM_PLAYER_PROFILES=profiles, MPC_PLUGIN_FOLDER=plugin_folder,
               NAM_PLAYER_LINK_BIND="127.0.0.1", NAM_PLAYER_LINK_PORT=str(PORT))
    env.pop("NAM_PLAYER_SYNC", None)
    cmds = T.base(0.0, 0.0, T.Q_FULL) + ["block=128", "live=%d" % (seconds * T.SR)]
    # The desktop companion, told to look for the plugin's link on this machine.
    tool_port = PORT + 1
    tool = subprocess.Popen([sys.executable, os.path.join(T.ROOT, "companion", "nam_card_loader.py"), "--port", str(tool_port)],
                            env=dict(os.environ, NAM_LOADER_LINK_HOSTS="127.0.0.1", NAM_LOADER_LINK_PORT=str(PORT), T3K_PUBLISHABLE_KEY=""),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    proc = subprocess.Popen([host, T.LIB, os.path.join(tmp, "in.f32"), os.path.join(tmp, "out.f32")] + cmds, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    start = time.time()
    marks = {}
    try:
        st = None
        for _ in range(60):
            try:
                st = status()
                break
            except Exception:
                time.sleep(0.1)
        check("the link answers while audio runs", st is not None and st["plugin"] == "NAM Player" and st["uploads"] is True, st and st["current"])
        check("status lists the card and what is playing",
              st["folders"] == [{"key": "user:Start", "name": "Start", "dir": "Start", "models": [{"name": "identity", "file": "identity.nam", "kind": "Linear"}]}]
              and st["current"]["file"] == "identity.nam", st["folders"])
        code, page = http("GET", "/", trusted=False)
        check("the page is served", code == 200 and b"<title>NAM Player</title>" in page)
        code, cat = http("GET", "/catalog.txt", trusted=False)
        check("the community catalogue is served from the plugin's Content folder", code == 200 and len(cat.decode().split("\n")) >= 259)

        time.sleep(max(0, 2.0 - (time.time() - start)))
        marks["upload_a2"] = time.time() - start
        code, r = http("PUT", "/api/profile?folder=Sent%20by%20link&name=Demo%20A2.nam&select=1&about=from%20a%20test", a2)
        check("a profile sent over the link is saved under NAM Profiles", code == 200 and r == {"folder": "Sent by link", "file": "Demo A2.nam", "already_there": False}
              and open(os.path.join(profiles, "Sent by link", "Demo A2.nam"), "rb").read() == a2, r)
        check("its source note is kept beside it", "from a test" in open(os.path.join(profiles, "Sent by link", "ABOUT.txt")).read())
        time.sleep(0.5)
        st = status()
        check("... and it is selected at once, without touching a knob", st["current"]["key"] == "user:Sent by link" and st["current"]["file"] == "Demo A2.nam"
              and [f["name"] for f in st["folders"]] == ["Sent by link", "Start"], st["current"])

        code, r2 = http("PUT", "/api/profile?folder=Sent%20by%20link&name=Demo%20A2.nam", a2)
        code, r3 = http("PUT", "/api/profile?folder=Sent%20by%20link&name=Demo%20A2.nam", a1)
        check("the same file again is recognised; a different file with the same name is kept beside it",
              r2.get("already_there") is True and r3.get("file") == "Demo A2 2.nam" and status()["current"]["file"] == "Demo A2.nam", [r2, r3])

        time.sleep(max(0, 4.5 - (time.time() - start)))
        marks["select_identity"] = time.time() - start
        code, r = http("POST", "/api/select?key=user%3AStart&file=identity.nam")
        time.sleep(0.4)
        check("a profile on the card can be auditioned from the page", code == 200 and status()["current"]["file"] == "identity.nam", r)
        code, r = http("POST", "/api/param?id=drive&value=6")
        code2, r2 = http("POST", "/api/param?id=model&value=0.5")
        code3, r3 = http("POST", "/api/param?id=mix&value=40", trusted=False)
        st = status()
        check("the page's controls move the real parameters (Drive to +6 dB); Model and unsigned requests are refused",
              code == 200 and code2 == 400 and code3 == 403 and st["params"]["drive"]["value"] == 6.0 and st["params"]["mix"]["value"] == 100.0, st["params"]["drive"])
        http("POST", "/api/param?id=drive&value=0")
        code, r = http("POST", "/api/capture?seconds=0.5")
        time.sleep(1.0)
        code2, wav = http("GET", "/api/capture.wav", trusted=False)
        cap = np.frombuffer(wav[44:], dtype=np.float32).reshape(-1, 4) if code2 == 200 else np.zeros((1, 4))
        # identity model: its output is its input 64 samples later, and so is the plugin's output
        lag = float(np.max(np.abs(cap[T.LAT + 2000:, 2] - cap[2000:-T.LAT, 1]))) if len(cap) > 4000 else 1.0
        check("a capture records what went into the model and what came out (identity profile: equal, 64 samples apart)",
              code == 200 and code2 == 200 and wav[:4] == b"RIFF" and len(cap) == int(0.5 * T.SR) and lag < 1e-6
              and float(np.sqrt(np.mean(cap[:, 1] ** 2))) > 0.1 and "engine" in st and st["rate"] == 44100, "%d frames, largest difference %.2g" % (len(cap), lag))
        # --- TONE3000 sign-in plumbing (the sign-in itself happens between the browser and tone3000.com)
        link = http("GET", "/api/link", trusted=False)[1]
        code, r = http("POST", "/api/t3k/key?key=t3k_cs_secret")
        code2, r2 = http("POST", "/api/t3k/key?key=t3k_pub_abc123", trusted=False)
        code3, r3 = http("POST", "/api/t3k/key?key=t3k_pub_abc123")
        key = http("GET", "/api/t3k/key", trusted=False)[1]
        check("TONE3000: the publishable key is kept on the card; a secret key and unsigned requests are refused",
              link == {"address": "127.0.0.1", "port": PORT} and code == 400 and code2 == 403 and code3 == 200 and key == {"key": "t3k_pub_abc123"}
              and open(os.path.join(profiles, ".tone3000-key")).read() == "t3k_pub_abc123" and status()["folders"][0]["key"] != "user:", [link, r, key])
        p0 = http("GET", "/api/t3k/poll?state=S1", trusted=False)[1]
        code, page = http("GET", "/t3k/callback?code=CODE9&state=S1&tone_id=42", trusted=False)
        p1 = http("GET", "/api/t3k/poll?state=WRONG", trusted=False)[1]
        p2 = http("GET", "/api/t3k/poll?state=S1", trusted=False)[1]
        p3 = http("GET", "/api/t3k/poll?state=S1", trusted=False)[1]
        check("TONE3000: the answer sent to /t3k/callback is handed once, and only to the page that knows its state",
              p0 == {"pending": True} and code == 200 and b"NAM Player has what it needs" in page and p1 == {"pending": True}
              and p2 == {"code": "CODE9", "tone_id": "42", "error": "", "canceled": False} and p3 == {"pending": True}, p2)
        http("POST", "/api/t3k/key?key=")
        code, r = http("POST", "/api/select?key=user%3ANope&file=x.nam")
        check("selecting a profile that does not exist is refused", code == 404, r)

        # --- things that must be refused
        code, r = http("PUT", "/api/profile?folder=X&name=y.nam", a2, trusted=False)
        check("changes without the page's header are refused (other web sites cannot drive the link)", code == 403 and not os.path.exists(os.path.join(profiles, "X")), r)
        code, r = http("PUT", "/api/profile?folder=..%2F..%2Fescape&name=..%2F..%2Fevil.nam", a2)
        inside = code == 200 and os.path.isfile(os.path.join(profiles, r["folder"], r["file"])) and not os.path.exists(os.path.join(tmp, "evil.nam")) and not os.path.exists(os.path.join(tmp, "escape"))
        check("folder and file names cannot leave NAM Profiles", inside, r)
        code, r = http("PUT", "/api/profile?folder=X&name=page.nam", b"<html>Sign in to download</html>")
        check("something that is not a model file is refused and leaves nothing behind", code == 400 and not os.path.exists(os.path.join(profiles, "X", "page.nam"))
              and not os.path.exists(os.path.join(profiles, "X", ".upload.part")), r)
        outside = os.path.join(tmp, "card", "outside.nam")
        open(outside, "wb").write(a2)
        code, r = http("DELETE", "/api/profile?folder=..&file=outside.nam")
        code2, r2 = http("DELETE", "/api/profile?folder=Start&file=..%2F..%2Foutside.nam")
        check("only profiles inside NAM Profiles can be removed", code == 400 and code2 == 400 and os.path.isfile(outside), [r, r2])
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.sendall(b"GARBAGE\r\n\r\n")
        s.close()
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.sendall(b"PUT /api/profile?folder=X&name=cut.nam HTTP/1.1\r\nX-NAM-Player: 1\r\nContent-Length: 500000\r\n\r\n{\"architecture\"")
        s.close()                                                    # hang up in the middle of an upload
        time.sleep(0.3)
        check("garbage and an upload cut off half way do no harm", status()["plugin"] == "NAM Player" and not os.path.exists(os.path.join(profiles, "X", "cut.nam"))
              and not os.path.exists(os.path.join(profiles, "X", ".upload.part")))

        code, r = http("DELETE", "/api/profile?folder=Sent%20by%20link&file=Demo%20A2%202.nam")
        st = status()
        check("a profile can be removed; the one that is playing stays selected",
              code == 200 and not os.path.exists(os.path.join(profiles, "Sent by link", "Demo A2 2.nam")) and st["current"]["file"] == "identity.nam", st["current"])

        time.sleep(max(0, 7.0 - (time.time() - start)))
        marks["upload_a1"] = time.time() - start
        # The second profile goes through the desktop companion, which has found the link by now.
        tool_status = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/status" % tool_port, timeout=10).read())
        check("the companion finds the running plugin and offers it as the place to send profiles",
              bool(tool_status["card"]) and tool_status["card"].get("live") is True and tool_status["card"]["path"] == BASE, tool_status["card"] and tool_status["card"]["path"])
        req = urllib.request.Request("http://127.0.0.1:%d/api/upload?folder=Via%%20companion&name=Classic.nam" % tool_port, data=a1,
                                     headers={"X-NAM-Card-Loader": "1"}, method="POST")
        r = json.loads(urllib.request.urlopen(req, timeout=30).read())
        time.sleep(0.5)
        st = status()
        check("a profile sent through the companion lands on the MPC's card and takes over",
              r.get("playing") is True and st["current"]["file"] == "Classic.nam" and st["current"]["folder"] == "Via companion"
              and os.path.isfile(os.path.join(profiles, "Via companion", "Classic.nam")), [r, st["current"]])

        out, err = proc.communicate(timeout=30)
        check("the host ran to the end and unloaded the plugin cleanly", proc.returncode == 0, err.decode()[-300:])
    finally:
        if proc.poll() is None:
            proc.kill()
        tool.terminate()

    # --- what the audio did. identity = input delayed 64 samples; a model = something else.
    y = np.fromfile(os.path.join(tmp, "out.f32"), dtype=np.float32).reshape(-1, 2).astype(np.float64)[:, 0]
    ref = np.concatenate([np.zeros(T.LAT), sine[:, 0]])[: len(y)]

    def differs(t0, t1):
        a, b = y[int(t0 * T.SR): int(t1 * T.SR)], T.dc_block(ref)[int(t0 * T.SR): int(t1 * T.SR)]
        return float(np.sqrt(np.mean((a - b) ** 2) / np.mean(b ** 2)))
    d = [differs(1.0, 1.8), differs(marks["upload_a2"] + 1.0, marks["select_identity"] - 0.2),
         differs(marks["select_identity"] + 1.0, marks["upload_a1"] - 0.2), differs(marks["upload_a1"] + 1.0, seconds - 0.2)]
    check("audio: pass-through, then the sent A2 profile, then pass-through again, then the sent A1 profile",
          d[0] < 0.01 and d[1] > 0.1 and d[2] < 0.01 and d[3] > 0.1 and abs(d[1] - d[3]) > 0.01, ["%.3f" % v for v in d])
    check("audio stayed clean throughout (finite, no jump larger than the signal's own slope allows)",
          bool(np.all(np.isfinite(y))) and float(np.max(np.abs(np.diff(y)))) < 0.6, "largest step %.3f" % float(np.max(np.abs(np.diff(y)))))

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
