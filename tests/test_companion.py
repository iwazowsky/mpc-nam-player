#!/usr/bin/env python3
"""Tests for companion/nam_card_loader.py against a pretend TONE3000 and a pretend GitHub.

    python3 tests/test_companion.py

The pretend TONE3000 implements what the real API documents: the authorize redirect of
the "select tone" flow, the PKCE token exchange (it verifies the code challenge), tone
and model listings, and a model download that needs the Bearer token. No network needed.
"""

import base64
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = open(os.path.join(ROOT, "tests", "fixtures", "wavenet.nam"), "rb").read()
MOCK_PORT, TOOL_PORT = 18931, 18932
seen = {"challenge": None, "pkce_ok": None, "bearer": [], "token_form": None}
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print("%s  %s%s" % ("PASS" if ok else "FAIL", name, ("  (" + str(detail) + ")") if detail else ""))


class Mock(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, obj, status=200, raw=None):
        body = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authed(self):
        ok = self.headers.get("Authorization") == "Bearer ACCESS-1"
        seen["bearer"].append(ok)
        return ok

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        if url.path == "/api/v1/oauth/authorize":
            seen["challenge"] = q["code_challenge"][0]
            seen["authorize"] = {k: v[0] for k, v in q.items()}
            back = q["redirect_uri"][0] + "?" + urllib.parse.urlencode({"code": "CODE-1", "state": q["state"][0], "tone_id": "42"})
            self.send_response(302)
            self.send_header("Location", back)
            self.end_headers()
        elif url.path == "/api/v1/tones/42":
            if not self.authed():
                return self.reply({"error": "unauthorized"}, 401)
            self.reply({"id": 42, "title": "Plexi 1959 Super Lead: all the knobs!", "gear": "amp-cab", "license": "cc-by",
                        "url": "https://www.tone3000.com/tones/42", "user": {"username": "captureguy", "display_name": None}})
        elif url.path == "/api/v1/models":
            if not self.authed():
                return self.reply({"error": "unauthorized"}, 401)
            base = "http://localhost:%d" % MOCK_PORT
            self.reply({"data": [
                {"id": 1, "name": "Plexi Bright Channel Volume 7 SM57", "size": "standard", "architecture_version": "2", "tone_id": 42, "model_url": base + "/files/1.nam"},
                {"id": 2, "name": "Plexi Normal Channel Volume 4", "size": "standard", "architecture_version": "2", "tone_id": 42, "model_url": base + "/files/2.nam"},
                {"id": 3, "name": "not a model", "size": "standard", "architecture_version": "2", "tone_id": 42, "model_url": base + "/files/3.nam"}],
                "page": 1, "page_size": 300, "total": 3, "total_pages": 1})
        elif url.path in ("/files/1.nam", "/files/2.nam"):
            if not self.authed():
                return self.reply({"error": "unauthorized"}, 401)
            self.reply(None, raw=MODEL)
        elif url.path == "/files/3.nam":
            self.reply(None, raw=b"<html>Sign in</html>")
        elif url.path.startswith("/raw/"):
            self.reply(None, raw=MODEL)
        else:
            self.reply({"error": "nope"}, 404)

    def do_POST(self):
        form = urllib.parse.parse_qs(self.rfile.read(int(self.headers["Content-Length"])).decode())
        seen["token_form"] = {k: v[0] for k, v in form.items()}
        verifier = form.get("code_verifier", [""])[0]
        challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
        seen["pkce_ok"] = challenge == seen["challenge"] and form.get("code") == ["CODE-1"]
        if not seen["pkce_ok"]:
            return self.reply({"error": "invalid_grant"}, 400)
        self.reply({"access_token": "ACCESS-1", "refresh_token": "REFRESH-1", "expires_in": 3600, "token_type": "bearer"})


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def get(url, follow=True):
    opener = urllib.request.build_opener() if follow else urllib.request.build_opener(NoRedirect)
    try:
        with opener.open(url, timeout=20) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def post(path, obj=None, raw=None, header=True):
    data = raw if raw is not None else json.dumps(obj or {}).encode()
    req = urllib.request.Request("http://localhost:%d%s" % (TOOL_PORT, path), data=data,
                                 headers={"X-NAM-Card-Loader": "1"} if header else {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def main():
    tmp = tempfile.mkdtemp(prefix="namloader-test-")
    card = os.path.join(tmp, "card")
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), Mock)
    threading.Thread(target=mock.serve_forever, daemon=True).start()
    env = dict(os.environ, T3K_API="http://localhost:%d" % MOCK_PORT, T3K_PUBLISHABLE_KEY="t3k_pub_test",
               NAM_LOADER_GITHUB_RAW="http://localhost:%d/raw/" % MOCK_PORT, NAM_LOADER_LINK_HOSTS="")
    tool = subprocess.Popen([sys.executable, os.path.join(ROOT, "companion", "nam_card_loader.py"), "--port", str(TOOL_PORT), "--card", card],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    base = "http://localhost:%d" % TOOL_PORT
    try:
        for _ in range(50):
            try:
                get(base + "/api/status")
                break
            except Exception:
                time.sleep(0.1)

        code, _, body = get(base + "/")
        check("page is served", code == 200 and b"NAM Card Loader" in body)
        st = json.loads(get(base + "/api/status")[2])
        check("status: card found, key set, not connected yet", st["card"]["path"] == card and st["t3k"] == {"key_set": True, "connected": False}, st["t3k"])

        # --- TONE3000 select flow: tool -> authorize -> callback -> token exchange -> page with the tone id
        code, headers, _ = get(base + "/t3k/start?arch=2&gears=amp-cab", follow=False)
        auth_url = headers.get("Location", "")
        a = urllib.parse.parse_qs(urllib.parse.urlparse(auth_url).query)
        ok = (code == 302 and auth_url.startswith("http://localhost:%d/api/v1/oauth/authorize" % MOCK_PORT) and a["prompt"] == ["select_tone"]
              and a["client_id"] == ["t3k_pub_test"] and a["code_challenge_method"] == ["S256"] and a["format"] == ["nam"] and a["architecture"] == ["2"]
              and a["gears"] == ["amp-cab"] and a["redirect_uri"] == [base + "/t3k/callback"] and a["response_type"] == ["code"])
        check("TONE3000: authorize URL carries the select-tone flow, PKCE S256 and a loopback redirect", ok, auth_url[:90])
        code, headers, _ = get(auth_url, follow=False)                 # TONE3000 sends the browser back
        code, headers, _ = get(headers["Location"], follow=False)      # the tool's callback
        check("TONE3000: callback exchanges the code (PKCE verifier matches) and returns to the page with the tone",
              code == 302 and headers["Location"] == "/#tone=42&arch=2" and seen["pkce_ok"] is True
              and seen["token_form"]["grant_type"] == "authorization_code" and seen["token_form"]["client_id"] == "t3k_pub_test"
              and seen["token_form"]["redirect_uri"] == base + "/t3k/callback", headers.get("Location"))
        code, headers, _ = get(base + "/t3k/callback?code=CODE-1&state=forged", follow=False)
        check("TONE3000: a callback with the wrong state is refused", "error=" in headers.get("Location", ""), headers.get("Location"))

        tone = json.loads(get(base + "/api/t3k/tone?id=42&arch=2")[2])
        check("TONE3000: tone and models listed, with short names for the MPC",
              tone["title"].startswith("Plexi") and len(tone["models"]) == 3 and all(len(m["suggested_name"]) <= 16 for m in tone["models"])
              and len(tone["suggested_folder"]) <= 18, [tone["suggested_folder"]] + [m["suggested_name"] for m in tone["models"]])

        code, r = post("/api/t3k/install", {"tone_id": 42, "arch": "2", "folder": "Plexi", "models": [{"id": 1, "name": "Bright 7"}, {"id": 2, "name": ""}]})
        files = sorted(os.listdir(os.path.join(card, "NAM Profiles", "Plexi"))) if code == 200 else []
        about = open(os.path.join(card, "NAM Profiles", "Plexi", "ABOUT.txt")).read() if "ABOUT.txt" in files else ""
        check("TONE3000: ticked models are downloaded with the Bearer token into NAM Profiles/<folder>/",
              code == 200 and files == ["ABOUT.txt", "Bright 7.nam", "Plexi Normal.nam"] and all(seen["bearer"])
              and open(os.path.join(card, "NAM Profiles", "Plexi", "Bright 7.nam"), "rb").read() == MODEL, files)
        check("TONE3000: creator and licence are kept beside the files", "captureguy" in about and "cc-by" in about and "tone3000.com/tones/42" in about)
        code, r = post("/api/t3k/install", {"tone_id": 42, "arch": "2", "folder": "Plexi", "models": [{"id": 3, "name": "junk"}]})
        check("a download that is not a model file is refused, nothing is written",
              code == 400 and "not a .nam" in r.get("error", "") and not os.path.exists(os.path.join(card, "NAM Profiles", "Plexi", "junk.nam")), r)

        # --- GitHub collection
        listing = json.loads(get(base + "/api/github/list")[2])
        first = listing["files"][0]
        check("GitHub: the collection is listed (259 captures)", len(listing["files"]) == 259 and listing["license"] == "GPL-3.0", len(listing["files"]))
        code, r = post("/api/github/install", {"folder": "Community", "files": [{"path": first["path"], "name": "KingKong"}]})
        check("GitHub: a capture is downloaded to the card with its licence note",
              code == 200 and os.path.isfile(os.path.join(card, "NAM Profiles", "Community", "KingKong.nam"))
              and "GPL-3.0" in open(os.path.join(card, "NAM Profiles", "Community", "ABOUT.txt")).read(), r)
        code, r = post("/api/github/install", {"folder": "x", "files": [{"path": "../../etc/passwd", "name": "p"}]})
        check("GitHub: only files of the collection can be requested", code == 400, r)

        # --- own files, safety
        code, r = post("/api/upload?folder=My%20Profiles&name=../../evil.nam", raw=MODEL)
        ok = code == 200 and r["file"] == "evil.nam" and os.path.isfile(os.path.join(card, "NAM Profiles", "My Profiles", "evil.nam")) and not os.path.exists(os.path.join(tmp, "evil.nam"))
        check("upload: file names cannot escape the folder", ok, r)
        other = MODEL.replace(b"0.5", b"0.6", 1) + b"\n"
        code, a1 = post("/api/upload?folder=Dup&name=amp.nam", raw=MODEL)
        code, a2 = post("/api/upload?folder=Dup&name=amp.nam", raw=other)
        code, a3 = post("/api/upload?folder=Dup&name=amp.nam", raw=MODEL)
        check("a different profile with the same short name is kept beside the first, not written over it",
              a1["file"] == "amp.nam" and a2["file"] == "amp 2.nam" and a3.get("already_there") is True
              and sorted(os.listdir(os.path.join(card, "NAM Profiles", "Dup"))) == ["amp 2.nam", "amp.nam"], [a1["file"], a2["file"], a3])
        for f in ("amp.nam", "amp 2.nam"):
            post("/api/delete", {"folder": "Dup", "file": f})
        names = [f["suggested_name"] for f in listing["files"]]
        check("GitHub: suggested short names fit the MPC and are all different", max(len(n) for n in names) <= 16 and len(set(n.lower() for n in names)) == len(names),
              "%d distinct of %d, e.g. %s" % (len(set(names)), len(names), names[:4]))
        code, r = post("/api/upload?folder=A&name=x.nam", raw=MODEL, header=False)
        check("requests without the page's header are refused (other web pages cannot drive the tool)", code == 401, r)
        outside = os.path.join(tmp, "outside.nam")
        open(outside, "wb").write(MODEL)
        code2, r2 = post("/api/delete", {"folder": "../..", "file": "outside.nam"})
        check("delete: only profiles inside NAM Profiles can be removed", code2 == 400 and os.path.isfile(outside), r2)
        code, r = post("/api/delete", {"folder": "My Profiles", "file": "evil.nam"})
        check("delete: removing the last profile also removes its empty folder", code == 200 and not os.path.exists(os.path.join(card, "NAM Profiles", "My Profiles")), r)

        st = json.loads(get(base + "/api/status")[2])
        folders = {f["folder"]: [x["name"] for x in f["files"]] for f in st["card"]["profiles"]}
        check("status lists what is on the card", folders == {"Community": ["KingKong.nam"], "Plexi": ["Bright 7.nam", "Plexi Normal.nam"]} and st["t3k"]["connected"], folders)
    finally:
        tool.terminate()
        mock.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d passed, %d failed" % (sum(results), len(results) - sum(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
