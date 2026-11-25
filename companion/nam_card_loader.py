#!/usr/bin/env python3
"""NAM Card Loader - browse NAM profiles online and put them on the MPC's card.

    python3 companion/nam_card_loader.py            then open http://localhost:8765

The MPC Sample has no internet connection, so NAM Player cannot download anything
itself. This is the other half: a small web app that runs on the computer the MPC
Sample is plugged into with its USB-C cable. It reaches the MPC in one of two ways:

  * Live: while NAM Player is loaded on the MPC, the plugin answers on the USB
    network link (http://mpc-sample.local:9626). This tool finds it within a few
    seconds; profiles you pick are saved to the MPC's card and start playing at once.
  * The card: put the MPC Sample in SD Card Access mode (Project screen); its card
    shows up as a drive; profiles are written into "NAM Profiles/<folder>/" on it.
    This is also how the plugin itself is installed.

Sources:
  * TONE3000 (tone3000.com), through its official API: you browse and audition tones
    on TONE3000's own pages ("select" flow), pick one, and choose which of its models
    to send to the card. Needs a free publishable API key from your TONE3000 account
    (Settings -> API Keys); nothing else to register, because the tool listens on
    localhost. Only what you pick is downloaded.
  * pelennor2170/NAM_models on GitHub: 259 community captures (GPL-3.0), no account.
  * Files you already have: drop .nam files on the page.

Standard library only; Python 3.9+. Options: --port N, --card PATH (use this folder as
the card instead of looking for one), --mpc HOST (the MPC's address on the USB link, if
it is not mpc-sample.local), --lan (also listen on the local network, so a phone on the
same Wi-Fi can be the screen; anyone on that network can then use the tool).
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import secrets
import shutil
import string
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG_PATH = os.path.join(HERE, "config.json")
PLUGIN_FOLDER_NAME = "Vazgen - VST - NAM Player"
PLUGIN_BUILD = os.path.join(ROOT, "plugins", "NAM Player", "build", PLUGIN_FOLDER_NAME)
PROFILES_DIR = "NAM Profiles"
T3K_API = os.environ.get("T3K_API", "https://www.tone3000.com").rstrip("/")
GITHUB_REPO = "pelennor2170/NAM_models"
GITHUB_RAW = os.environ.get("NAM_LOADER_GITHUB_RAW", "https://raw.githubusercontent.com/%s/main/" % GITHUB_REPO)
GITHUB_CATALOG = os.path.join(HERE, "catalog", "pelennor2170_NAM_models.txt")
USER_AGENT = "nam-card-loader/1.0"
MAX_MODEL_BYTES = 32 * 1024 * 1024

LINK_PORT = int(os.environ.get("NAM_LOADER_LINK_PORT", "9626"))   # the plugin's live link (src/LinkServer.h)
LINK_HOSTS = ["mpc-sample.local", "192.168.155.1"]     # how Akai's firmware guide says the MPC Sample is reached over USB

STATE = {
    "link": None,              # "http://host:9626" while NAM Player answers there
    "card_override": None,     # --card
    "chosen_card": None,       # picked in the UI
    "pkce": {},                # state -> {"verifier", "redirect_uri", "arch"}
    "tokens": None,            # {"access_token", "refresh_token", "expires_at"}
    "lock": threading.Lock(),
}


# ----------------------------------------------------------------------------- config

def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def save_config(cfg):
    with open(CONFIG_PATH, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)


# ----------------------------------------------------------------------------- the live link

def link_request(method, path, body=None, timeout=20):
    """A request to the plugin running on the MPC. Raises ValueError with the plugin's own message."""
    req = urllib.request.Request(STATE["link"] + path, data=body, method=method,
                                 headers={"X-NAM-Player": "1", "User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            return json.loads(res.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            message = json.loads(e.read().decode("utf-8")).get("error")
        except Exception:   # noqa: BLE001
            message = None
        raise ValueError(message or "the MPC refused (HTTP %d)" % e.code)
    except (urllib.error.URLError, OSError):
        STATE["link"] = None
        raise ValueError("the MPC is not answering any more: is NAM Player still loaded?")


def link_probe_forever(hosts):
    """Looks for NAM Player on the USB network link every few seconds (name lookups can be slow, so not on a request)."""
    while True:
        found = None
        for host in hosts:
            url = "http://%s:%d" % (host, LINK_PORT)
            try:
                with urllib.request.urlopen(url + "/api/status", timeout=2) as res:
                    if json.loads(res.read().decode("utf-8")).get("plugin") == "NAM Player":
                        found = url
                        break
            except Exception:   # noqa: BLE001 - not there, try the next address
                continue
        STATE["link"] = found
        time.sleep(4)


def is_link(card):
    return card["path"].startswith("http://")


# ----------------------------------------------------------------------------- the card

def candidate_volumes():
    roots = []
    if sys.platform == "darwin":
        base = "/Volumes"
        roots = [os.path.join(base, d) for d in sorted(os.listdir(base))] if os.path.isdir(base) else []
        roots = [r for r in roots if not os.path.islink(r)]          # skip the boot volume's alias
    elif os.name == "nt":
        roots = ["%s:\\" % c for c in string.ascii_uppercase[3:] if os.path.isdir("%s:\\" % c)]
    else:
        user = os.environ.get("USER", "")
        for base in ("/media/" + user, "/run/media/" + user, "/media", "/mnt"):
            if os.path.isdir(base):
                roots += [os.path.join(base, d) for d in sorted(os.listdir(base))]
    return [r for r in roots if os.path.isdir(r)]


def find_cards():
    """Mounted volumes, the ones that look like an MPC card (they have 'VST2 Plugins') first."""
    if STATE["card_override"]:
        return [{"path": STATE["card_override"], "mpc": True, "name": os.path.basename(STATE["card_override"]) or STATE["card_override"]}]
    cards = []
    if STATE["link"]:   # the plugin itself, reached over the USB network link: nothing to mount or eject
        cards.append({"path": STATE["link"], "mpc": True, "name": "MPC, live (NAM Player is running)"})
    for root in candidate_volumes():
        try:
            mpc = os.path.isdir(os.path.join(root, "VST2 Plugins")) or os.path.isdir(os.path.join(root, PROFILES_DIR))
            writable = os.access(root, os.W_OK)
        except OSError:
            continue
        if writable:
            cards.append({"path": root, "mpc": mpc, "name": os.path.basename(root.rstrip("/\\")) or root})
    cards.sort(key=lambda c: (not is_link(c), not c["mpc"], c["name"].lower()))
    return cards


def current_card():
    cards = find_cards()
    chosen = STATE["chosen_card"]
    for c in cards:
        if c["path"] == chosen:
            return c
    for c in cards:
        if c["mpc"]:
            return c
    return None


def safe_name(text, limit, fallback):
    """A file or folder name that is safe on FAT/exFAT and short enough for the MPC Sample's screen."""
    text = re.sub(r"[^A-Za-z0-9 _+\-().&']", " ", text or "")
    text = re.sub(r"\s+", " ", text).strip(" .")
    if len(text) > limit:
        cut = text[:limit]
        text = cut[: cut.rfind(" ")] if " " in cut[limit // 2:] else cut
        text = text.strip(" .-_")
    return text or fallback


def short_names(names, limit, strip_creator=False):
    """Short, distinguishable names for a list of captures.

    Captures that belong together usually share a long prefix ("Ceriatone King Kong
    chan2 60s ...", "Ceriatone King Kong Channel 1 ..."), so cutting each name to
    16 characters would make them all the same. Within a group that shares its first
    two words, the shared words are reduced to their initials: "CKK chan2 60s br"."""
    cleaned = []
    for n in names:
        n = re.sub(r"\.nam$", "", os.path.basename(n or ""), flags=re.I)
        if strip_creator:
            n = re.sub(r"^[A-Z][a-z]+ [A-Z]\.? (?=\S)", "", n)     # "George B Ceriatone ..." -> "Ceriatone ..."
        cleaned.append(safe_name(n, 200, "model").split(" "))
    groups = {}
    for i, words in enumerate(cleaned):
        groups.setdefault(" ".join(words[:2]).lower(), []).append(i)
    out = [None] * len(cleaned)
    for members in groups.values():
        common = 0
        if len(members) > 1:
            first = cleaned[members[0]]
            while common < len(first) - 1 and all(len(cleaned[m]) > common + 1 and cleaned[m][common].lower() == first[common].lower() for m in members):
                common += 1
        for m in members:
            words = cleaned[m]
            head = "".join(w[0].upper() for w in words[:common] if w) if common >= 2 else " ".join(words[:common])
            out[m] = safe_name((head + " " + " ".join(words[common:])).strip(), limit, "model")
    # Names that are still equal after shortening get a number, so every suggestion is distinct.
    count, seen = {}, set()
    for n in out:
        count[n.lower()] = count.get(n.lower(), 0) + 1
    numbered = {}
    for i, n in enumerate(out):
        if count[n.lower()] > 1:
            numbered[n.lower()] = numbered.get(n.lower(), 0) + 1
            n = "%s %d" % (n[: limit - 1 - len(str(numbered[n.lower()]))].rstrip(), numbered[n.lower()])
        while n.lower() in seen:
            n = n[: limit - 1] + "+"
            if n.lower() in seen:
                n = "%s%d" % (n[: limit - 4], i)
        seen.add(n.lower())
        out[i] = n
    return out


def profiles_root(card):
    return os.path.join(card["path"], PROFILES_DIR)


def list_profiles(card):
    out = []
    if is_link(card):
        for f in link_request("GET", "/api/status").get("folders", []):
            if f.get("dir") is not None:   # the plugin's own factory profiles are not ours to manage
                out.append({"folder": f["dir"], "files": [{"name": m["file"], "bytes": None} for m in f["models"]]})
        return out
    root = profiles_root(card)
    if not os.path.isdir(root):
        return out
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        rel = os.path.relpath(dirpath, root)
        if rel.count(os.sep) >= 3:
            dirnames[:] = []
        files = sorted(f for f in filenames if f.lower().endswith(".nam") and not f.startswith("."))
        if files:
            out.append({"folder": "" if rel == "." else rel.replace(os.sep, "/"),
                        "files": [{"name": f, "bytes": os.path.getsize(os.path.join(dirpath, f))} for f in files]})
    return out


def describe_model(data):
    """Returns (ok, description). Refuses anything that is not a NAM model file."""
    try:
        j = json.loads(data.decode("utf-8"))
        arch = j["architecture"]
        j["config"]
    except Exception:
        return False, "not a .nam model file"
    if arch == "SlimmableContainer":
        return True, "A2 (slimmable: Lite + Full)"
    if arch == "WaveNet":
        try:
            layers = j["config"]["layers"]
            ch = layers[0]["channels"]
            if len(layers) == 1 and "kernel_sizes" in layers[0]:
                return True, "A2 %s" % ("Lite" if ch == 3 else "Full" if ch == 8 else "custom")
            names = {16: "A1 standard", 12: "A1 lite", 8: "A1 feather", 4: "A1 nano"}
            return True, names.get(ch, "A1 custom WaveNet")
        except Exception:
            return True, "WaveNet"
    return True, str(arch)


def write_profile(card, folder, name, data, about_lines):
    ok, kind = describe_model(data)
    if not ok:
        raise ValueError(kind)
    folder = safe_name(folder, 24, "Profiles")
    name = safe_name(re.sub(r"\.nam$", "", name, flags=re.I), 24, "model")
    if is_link(card):   # hand it to the running plugin, which saves it to its card and starts playing it
        query = urllib.parse.urlencode({"folder": folder, "name": name, "select": "1", "about": "; ".join(about_lines or [])[:380]})
        done = link_request("PUT", "/api/profile?" + query, data, timeout=60)
        return {"folder": done["folder"], "file": done["file"], "kind": kind, "bytes": len(data),
                "already_there": bool(done.get("already_there")), "playing": True}
    target_dir = os.path.join(profiles_root(card), folder)
    os.makedirs(target_dir, exist_ok=True)
    # Never replace a different profile that happens to have the same short name.
    base, n = name, 1
    while True:
        path = os.path.join(target_dir, name + ".nam")
        if not os.path.exists(path):
            break
        with open(path, "rb") as fh:
            if fh.read() == data:
                return {"folder": folder, "file": name + ".nam", "kind": kind, "bytes": len(data), "already_there": True}
        n += 1
        name = "%s %d" % (base[:21], n)
    tmp = path + ".part"
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)
    if about_lines:      # keep creator and licence with the files, as the sources ask
        with open(os.path.join(target_dir, "ABOUT.txt"), "a", encoding="utf-8") as fh:
            fh.write("%s.nam\n" % name + "".join("    %s\n" % line for line in about_lines) + "\n")
    return {"folder": folder, "file": name + ".nam", "kind": kind, "bytes": len(data)}


def delete_profile(card, folder, file):
    if is_link(card):
        link_request("DELETE", "/api/profile?" + urllib.parse.urlencode({"folder": folder, "file": file}))
        return
    root = os.path.realpath(profiles_root(card))
    path = os.path.realpath(os.path.join(root, folder, file))
    if not path.startswith(root + os.sep) or not path.lower().endswith(".nam") or not os.path.isfile(path):
        raise ValueError("not a profile on the card")
    os.remove(path)
    parent = os.path.dirname(path)
    if parent != root and not [f for f in os.listdir(parent) if f.lower().endswith(".nam")]:
        about = os.path.join(parent, "ABOUT.txt")
        if os.path.isfile(about):
            os.remove(about)
        if not os.listdir(parent):
            os.rmdir(parent)


def install_plugin(card):
    if is_link(card):
        raise ValueError("the plugin cannot replace itself while it runs: use SD Card Access mode for that")
    if not os.path.isfile(os.path.join(PLUGIN_BUILD, "plugin.json")):
        raise ValueError("NAM Player has not been built yet: run  python3 build.py  first")
    dest = os.path.join(card["path"], "VST2 Plugins", PLUGIN_FOLDER_NAME)
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    shutil.copytree(PLUGIN_BUILD, dest, ignore=shutil.ignore_patterns(".DS_Store", "._*"))
    return dest


# ----------------------------------------------------------------------------- HTTP helpers

def http_request(url, data=None, headers=None, timeout=60):
    req = urllib.request.Request(url, data=data, headers=dict({"User-Agent": USER_AGENT}, **(headers or {})))
    with urllib.request.urlopen(req, timeout=timeout) as res:
        body = res.read(MAX_MODEL_BYTES + 1)
        if len(body) > MAX_MODEL_BYTES:
            raise ValueError("response too large")
        return body


# ----------------------------------------------------------------------------- TONE3000

def t3k_key():
    return os.environ.get("T3K_PUBLISHABLE_KEY") or load_config().get("t3k_publishable_key") or ""


def t3k_authorize_url(redirect_uri, arch, gears):
    verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b"=").decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    with STATE["lock"]:
        STATE["pkce"] = {state: {"verifier": verifier, "redirect_uri": redirect_uri, "arch": arch}}   # one flow at a time
    params = {"client_id": t3k_key(), "redirect_uri": redirect_uri, "response_type": "code",
              "code_challenge": challenge, "code_challenge_method": "S256", "state": state,
              "prompt": "select_tone", "format": "nam", "architecture": arch, "preview": "true", "menubar": "true"}
    if gears:
        params["gears"] = gears
    return T3K_API + "/api/v1/oauth/authorize?" + urllib.parse.urlencode(params)


def t3k_store_tokens(body):
    data = json.loads(body.decode("utf-8"))
    STATE["tokens"] = {"access_token": data["access_token"], "refresh_token": data.get("refresh_token"),
                       "expires_at": time.time() + float(data.get("expires_in", 3600))}


def t3k_exchange(code, state):
    with STATE["lock"]:
        flow = STATE["pkce"].pop(state, None)
    if not flow:
        raise ValueError("state mismatch (start the TONE3000 flow again)")
    form = urllib.parse.urlencode({"grant_type": "authorization_code", "code": code, "code_verifier": flow["verifier"],
                                   "redirect_uri": flow["redirect_uri"], "client_id": t3k_key()}).encode()
    t3k_store_tokens(http_request(T3K_API + "/api/v1/oauth/token", data=form,
                                  headers={"Content-Type": "application/x-www-form-urlencoded"}))
    return flow["arch"]


def t3k_token():
    tok = STATE["tokens"]
    if not tok:
        raise PermissionError("not connected to TONE3000")
    if tok["expires_at"] - time.time() < 60 and tok.get("refresh_token"):
        form = urllib.parse.urlencode({"grant_type": "refresh_token", "refresh_token": tok["refresh_token"],
                                       "client_id": t3k_key()}).encode()
        try:
            t3k_store_tokens(http_request(T3K_API + "/api/v1/oauth/token", data=form,
                                          headers={"Content-Type": "application/x-www-form-urlencoded"}))
        except urllib.error.HTTPError:
            STATE["tokens"] = None
            raise PermissionError("the TONE3000 session has expired: connect again")
    return STATE["tokens"]["access_token"]


def t3k_get(path_or_url):
    """Authenticated GET. The token is only ever sent to the TONE3000 API origin."""
    path = re.sub(r"^https?://[^/]+", "", path_or_url)
    return http_request(T3K_API + path, headers={"Authorization": "Bearer " + t3k_token()})


def t3k_tone(tone_id, arch):
    tone_id = str(int(tone_id))
    tone = json.loads(t3k_get("/api/v1/tones/%s?architecture=%s" % (tone_id, urllib.parse.quote(arch))))
    models = json.loads(t3k_get("/api/v1/models?tone_id=%s&architecture=%s&page_size=300" % (tone_id, urllib.parse.quote(arch))))
    user = tone.get("user") or {}
    rows = models.get("data", [])
    names = short_names([m.get("name") for m in rows], 16)
    return {
        "id": tone.get("id"), "title": tone.get("title"), "gear": tone.get("gear"), "license": tone.get("license"),
        "url": tone.get("url"), "creator": user.get("display_name") or user.get("username"),
        "suggested_folder": safe_name(tone.get("title"), 18, "TONE3000"),
        "models": [{"id": m.get("id"), "name": m.get("name"), "size": m.get("size"),
                    "architecture": m.get("architecture_version"), "model_url": m.get("model_url"),
                    "suggested_name": names[i]} for i, m in enumerate(rows)],
    }


# ----------------------------------------------------------------------------- GitHub collection

_github_list = None


def github_list():
    global _github_list
    if _github_list is None:
        names = []
        try:   # the live list, if GitHub's anonymous API allowance (60 requests an hour) permits
            tree = json.loads(http_request("https://api.github.com/repos/%s/git/trees/HEAD?recursive=1" % GITHUB_REPO, timeout=15))
            names = [t["path"] for t in tree.get("tree", []) if t.get("path", "").lower().endswith(".nam")]
        except Exception:
            names = []
        if not names:   # the collection has not changed since March 2023: a saved list is as good
            with open(GITHUB_CATALOG, "r", encoding="utf-8") as fh:
                names = [line.rstrip("\n") for line in fh if line.strip()]
        names = sorted(names, key=str.lower)
        short = short_names(names, 16, strip_creator=True)
        _github_list = [{"path": n, "suggested_name": short[i]} for i, n in enumerate(names)]
    return _github_list


# ----------------------------------------------------------------------------- web server

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "NamCardLoader/1.0"

    def log_message(self, fmt, *args):
        pass

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def fail(self, exc):
        status = 401 if isinstance(exc, PermissionError) else 400
        if isinstance(exc, urllib.error.HTTPError):
            msg = {401: "TONE3000 refused the request: connect again", 403: "TONE3000 says this is not allowed for your key (403)",
                   404: "not found at the source (404)", 429: "the source is rate-limiting requests: wait a minute"}.get(exc.code, "the source answered HTTP %d" % exc.code)
        elif isinstance(exc, urllib.error.URLError):
            msg = "cannot reach the source (%s)" % exc.reason
        else:
            msg = str(exc) or exc.__class__.__name__
        self.send_json({"error": msg}, status)

    def origin(self):
        return "http://" + self.headers.get("Host", "localhost")

    def read_body(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n > MAX_MODEL_BYTES:
            raise ValueError("file too large")
        return self.rfile.read(n)

    def need_card(self):
        card = current_card()
        if not card:
            raise ValueError("no card: put the MPC Sample in SD Card Access mode and press Rescan")
        return card

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        try:
            if url.path == "/":
                body = PAGE.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif url.path == "/api/status":
                cards = find_cards()
                card = current_card()
                info = None
                if card and is_link(card):
                    info = {"path": card["path"], "name": card["name"], "free_mb": None, "live": True,
                            "plugin_installed": True, "profiles": list_profiles(card)}
                elif card:
                    usage = shutil.disk_usage(card["path"])
                    plugin = os.path.isfile(os.path.join(card["path"], "VST2 Plugins", PLUGIN_FOLDER_NAME, "plugin.json"))
                    info = {"path": card["path"], "name": card["name"], "free_mb": usage.free // (1024 * 1024),
                            "plugin_installed": plugin, "profiles": list_profiles(card)}
                self.send_json({"cards": cards, "card": info, "plugin_built": os.path.isfile(os.path.join(PLUGIN_BUILD, "plugin.json")),
                                "t3k": {"key_set": bool(t3k_key()), "connected": STATE["tokens"] is not None}})
            elif url.path == "/t3k/start":
                if not t3k_key():
                    raise ValueError("enter your TONE3000 publishable key first")
                arch = q.get("arch", ["2"])[0]
                if arch not in ("1", "2", "custom"):
                    arch = "2"
                gears = re.sub(r"[^a-z_\-]", "", q.get("gears", [""])[0])
                self.send_response(302)
                self.send_header("Location", t3k_authorize_url(self.origin() + "/t3k/callback", arch, gears))
                self.end_headers()
            elif url.path == "/t3k/callback":
                target = "/"
                if q.get("code") and q.get("state"):
                    arch = t3k_exchange(q["code"][0], q["state"][0])
                    if q.get("tone_id"):
                        target = "/#tone=%s&arch=%s" % (urllib.parse.quote(q["tone_id"][0]), arch)
                elif q.get("error"):
                    target = "/#error=" + urllib.parse.quote(q["error"][0])
                self.send_response(302)
                self.send_header("Location", target)
                self.end_headers()
            elif url.path == "/api/t3k/tone":
                self.send_json(t3k_tone(q["id"][0], q.get("arch", ["2"])[0]))
            elif url.path == "/api/github/list":
                self.send_json({"repo": GITHUB_REPO, "license": "GPL-3.0", "files": github_list()})
            else:
                self.send_json({"error": "not found"}, 404)
        except Exception as exc:   # noqa: BLE001 - every failure becomes a message on the page
            if url.path.startswith("/t3k/"):
                self.send_response(302)
                self.send_header("Location", "/#error=" + urllib.parse.quote(str(exc) or exc.__class__.__name__))
                self.end_headers()
            else:
                self.fail(exc)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        try:
            # Only this page may change things: other web pages cannot send this header.
            if self.headers.get("X-NAM-Card-Loader") != "1":
                raise PermissionError("request did not come from the NAM Card Loader page")
            body = self.read_body()
            if url.path == "/api/upload":
                card = self.need_card()
                self.send_json(write_profile(card, q.get("folder", ["My Profiles"])[0], q.get("name", ["model"])[0], body, None))
                return
            data = json.loads(body.decode("utf-8") or "{}")
            if url.path == "/api/card":
                STATE["chosen_card"] = data.get("path")
                self.send_json({"ok": True})
            elif url.path == "/api/t3k/key":
                key = str(data.get("key", "")).strip()
                if key and not key.startswith("t3k_pub_"):
                    raise ValueError("that is not a publishable key: it must start with t3k_pub_ (never paste the secret key)")
                cfg = load_config()
                cfg["t3k_publishable_key"] = key
                save_config(cfg)
                STATE["tokens"] = None
                self.send_json({"ok": True})
            elif url.path == "/api/t3k/install":
                card = self.need_card()
                tone = t3k_tone(data["tone_id"], str(data.get("arch", "2")))
                wanted = {int(m["id"]): m.get("name") for m in data.get("models", [])}
                about = ["from TONE3000: %s" % (tone.get("url") or "https://www.tone3000.com"),
                         "tone: %s   creator: %s   licence: %s" % (tone.get("title"), tone.get("creator"), tone.get("license"))]
                done = []
                for m in tone["models"]:
                    if m["id"] in wanted:
                        blob = t3k_get(m["model_url"])
                        done.append(write_profile(card, data.get("folder") or tone["suggested_folder"],
                                                  wanted[m["id"]] or m["suggested_name"], blob, about))
                self.send_json({"installed": done})
            elif url.path == "/api/github/install":
                card = self.need_card()
                known = {f["path"] for f in github_list()}
                done = []
                for item in data.get("files", []):
                    if item.get("path") not in known:
                        raise ValueError("not a file of the collection")
                    blob = http_request(GITHUB_RAW + urllib.parse.quote(item["path"]))
                    about = ["from https://github.com/%s (GPL-3.0)" % GITHUB_REPO, "original file: %s" % item["path"]]
                    done.append(write_profile(card, data.get("folder") or "Community", item.get("name") or item["path"], blob, about))
                self.send_json({"installed": done})
            elif url.path == "/api/delete":
                delete_profile(self.need_card(), str(data.get("folder", "")), str(data.get("file", "")))
                self.send_json({"ok": True})
            elif url.path == "/api/install-plugin":
                self.send_json({"installed_to": install_plugin(self.need_card())})
            else:
                self.send_json({"error": "not found"}, 404)
        except Exception as exc:   # noqa: BLE001
            self.fail(exc)


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>NAM Card Loader</title>
<style>
  :root { color-scheme: light dark; --bg:#f4f1ea; --panel:#fffdf8; --ink:#1d1b17; --muted:#6d675c; --line:#d9d2c3; --accent:#b4530a; --ok:#2f6b3a; --bad:#a3271c; }
  @media (prefers-color-scheme: dark) { :root { --bg:#181613; --panel:#211e1a; --ink:#ece6d8; --muted:#a19887; --line:#3a352d; --accent:#f0a04b; --ok:#7cc48a; --bad:#f08a7c; } }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--ink); font:15px/1.45 ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif; }
  main { max-width: 980px; margin: 0 auto; padding: 20px 16px 60px; }
  h1 { font-size: 22px; margin: 0 0 2px; letter-spacing: .01em; }
  h2 { font-size: 15px; margin: 0 0 10px; text-transform: uppercase; letter-spacing: .08em; color: var(--muted); }
  p { margin: 6px 0; } .muted { color: var(--muted); } .small { font-size: 13px; }
  section { background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 16px; margin-top: 16px; }
  button, select, input[type=text] { font: inherit; color: inherit; background: transparent; border: 1px solid var(--line); border-radius: 7px; padding: 7px 11px; }
  button { cursor: pointer; } button.primary { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }
  @media (prefers-color-scheme: dark) { button.primary { color: #1b1206; } }
  button:disabled { opacity: .45; cursor: default; }
  input[type=text] { width: 100%; } .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .grow { flex: 1 1 220px; }
  table { width: 100%; border-collapse: collapse; table-layout: fixed; } td { overflow-wrap: anywhere; } th.size, td.size { width: 110px; } td input[type=text] { width: 100%; min-width: 0; } th.name, td.name { width: 210px; } th.tick, td.tick { width: 30px; }
  td, th { text-align: left; padding: 6px 6px; border-top: 1px solid var(--line); vertical-align: middle; }
  th { font-size: 12px; text-transform: uppercase; letter-spacing: .06em; color: var(--muted); border-top: 0; }
  td.num { font-variant-numeric: tabular-nums; white-space: nowrap; }
  .scroll { max-height: 340px; overflow: auto; border: 1px solid var(--line); border-radius: 8px; margin-top: 10px; }
  .pill { display:inline-block; font-size:12px; padding:1px 8px; border-radius:99px; border:1px solid var(--line); color:var(--muted); }
  .ok { color: var(--ok); } .bad { color: var(--bad); }
  #toast { position: fixed; left: 50%; bottom: 18px; transform: translateX(-50%); background: var(--ink); color: var(--bg); padding: 9px 14px; border-radius: 8px; max-width: 92vw; display: none; }
  .drop { border: 1.5px dashed var(--line); border-radius: 9px; padding: 18px; text-align: center; color: var(--muted); }
  .drop.over { border-color: var(--accent); color: var(--accent); }
  a { color: var(--accent); }
</style></head><body><main>
<h1>NAM Card Loader</h1>
<p class="muted">Puts Neural Amp Modeler profiles on the MPC's card, where the NAM Player plugin finds them. Names are kept short because the MPC Sample shows about nine characters per knob.</p>

<section><h2>1 &middot; The MPC</h2><div id="card"></div></section>

<section><h2>2 &middot; TONE3000</h2>
  <div id="t3k"></div><div id="tone"></div>
  <p class="small muted">Powered by <a href="https://www.tone3000.com" target="_blank" rel="noopener">TONE3000</a>. You browse and audition on TONE3000; only the models you tick are downloaded. Each folder gets an ABOUT.txt naming the creator and licence.</p>
</section>

<section><h2>3 &middot; Community collection on GitHub</h2>
  <p class="small muted">pelennor2170/NAM_models: 259 captures shared by the NAM community, GPL-3.0. Older (A1) profiles: heavier on the CPU than A2.</p>
  <div class="row"><input class="grow" type="text" id="ghq" placeholder="Filter, e.g. fender, 5150, jcm"><input type="text" id="ghfolder" value="Community" style="width:170px" title="Folder on the card"><button class="primary" id="ghgo" disabled>Send to card</button></div>
  <div class="scroll"><table id="gh"></table></div>
</section>

<section><h2>4 &middot; Files you already have</h2>
  <div class="row" style="margin-bottom:10px"><span>Folder on the card</span><input type="text" id="upfolder" value="My Profiles" style="width:200px"></div>
  <div class="drop" id="drop">Drop .nam files here, or <label style="color:var(--accent);cursor:pointer">choose files<input type="file" id="pick" accept=".nam" multiple hidden></label></div>
</section>

<section><h2>On the card now</h2><div id="lib"></div></section>
</main><div id="toast"></div>
<script>
const $ = id => document.getElementById(id);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let status = null, gh = [], picked = new Map();
function toast(msg, bad) { const t = $('toast'); t.textContent = msg; t.style.display = 'block'; t.style.background = bad ? 'var(--bad)' : ''; clearTimeout(toast.t); toast.t = setTimeout(() => t.style.display = 'none', bad ? 7000 : 3500); }
async function api(path, body, raw) {
  const opt = body === undefined ? {} : { method: 'POST', headers: { 'X-NAM-Card-Loader': '1' }, body: raw ? body : JSON.stringify(body) };
  const res = await fetch(path, opt); const j = await res.json().catch(() => ({ error: 'bad answer from the tool' }));
  if (!res.ok || j.error) throw new Error(j.error || ('HTTP ' + res.status)); return j;
}
async function refresh() {
  status = await api('/api/status'); const c = status.card;
  let h = '';
  if (!c) h = '<p class="bad">No MPC found.</p><p class="small muted"><b>Live:</b> connect the MPC Sample with USB-C, power it on normally and load NAM Player as a Knob FX; it is found within a few seconds. <b>Or the card:</b> Project screen &rarr; SD Card Access, then press Rescan (needed once, to install the plugin).</p>';
  else if (c.live) h = `<p><b>${esc(c.name)}</b> <span class="muted small">${esc(c.path)}</span></p><p class="small ok">Profiles you send are saved to the MPC's card and start playing at once. Nothing to eject.</p>`;
  else h = `<p><b>${esc(c.name)}</b> <span class="muted small">${esc(c.path)} &middot; ${c.free_mb} MB free</span></p>` +
    `<p class="small ${c.plugin_installed ? 'ok' : 'bad'}">${c.plugin_installed ? 'NAM Player is installed on this card.' : 'NAM Player is not on this card yet.'}</p>`;
  h += '<div class="row"><button id="rescan">Rescan</button>';
  if (status.cards.length > 1) h += '<select id="cards">' + status.cards.map(k => `<option value="${esc(k.path)}" ${c && k.path === c.path ? 'selected' : ''}>${esc(k.name)}${k.mpc ? '' : ' (no MPC folders)'}</option>`).join('') + '</select>';
  else if (!c && status.cards.length === 1) h += `<button id="useonly" data-p="${esc(status.cards[0].path)}">Use "${esc(status.cards[0].name)}" anyway</button>`;
  if (c && !c.live && status.plugin_built) h += `<button id="plug">${c.plugin_installed ? 'Update' : 'Install'} NAM Player on this card</button>`;
  h += '</div>' + (c && !c.live ? '<p class="small muted">When you are done: eject the card on the computer first, then leave SD Card Access mode on the MPC Sample.</p>' : '');
  $('card').innerHTML = h;
  $('rescan').onclick = () => refresh().catch(e => toast(e.message, true));
  if ($('cards')) $('cards').onchange = async e => { await api('/api/card', { path: e.target.value }); refresh(); };
  if ($('useonly')) $('useonly').onclick = async e => { await api('/api/card', { path: e.target.dataset.p }); refresh(); };
  if ($('plug')) $('plug').onclick = async () => { if (!confirm('Copy the NAM Player plugin folder to "VST2 Plugins" on this card? An older copy there is replaced.')) return; try { await api('/api/install-plugin', {}); toast('NAM Player copied to the card'); refresh(); } catch (e) { toast(e.message, true); } };
  renderT3k(); renderLib();
}
function renderT3k() {
  const t = status.t3k; let h = '';
  if (!t.key_set) h = '<p>Paste your TONE3000 <b>publishable</b> key (TONE3000 &rarr; Settings &rarr; API Keys; it starts with <code>t3k_pub_</code>). It is saved in companion/config.json on this computer.</p><div class="row"><input class="grow" type="text" id="key" placeholder="t3k_pub_..."><button class="primary" id="savekey">Save</button></div>';
  else h = `<div class="row"><select id="arch" title="Which kind of model to show"><option value="2">A2 profiles (lightest on the MPC)</option><option value="1">A1 profiles</option><option value="custom">Custom architectures</option></select>` +
    `<select id="gears"><option value="">Any gear</option><option value="amp-cab">Amp + cab</option><option value="amp">Amp heads</option><option value="pedal">Pedals</option><option value="outboard">Outboard</option></select>` +
    `<button class="primary" id="browse">Browse TONE3000</button><button id="forget">Change key</button><span class="small muted">${t.connected ? 'Connected.' : 'You sign in on TONE3000 when you browse.'}</span></div>`;
  $('t3k').innerHTML = h;
  if ($('savekey')) $('savekey').onclick = async () => { try { await api('/api/t3k/key', { key: $('key').value }); refresh(); } catch (e) { toast(e.message, true); } };
  if ($('browse')) $('browse').onclick = () => location.href = '/t3k/start?arch=' + $('arch').value + '&gears=' + $('gears').value;
  if ($('forget')) $('forget').onclick = async () => { await api('/api/t3k/key', { key: '' }); refresh(); };
}
async function showTone(id, arch) {
  try {
    const t = await api('/api/t3k/tone?id=' + encodeURIComponent(id) + '&arch=' + encodeURIComponent(arch));
    let h = `<p style="margin-top:14px"><b>${esc(t.title)}</b> <span class="muted small">by ${esc(t.creator)} &middot; ${esc(t.gear)} &middot; licence ${esc(t.license)}</span></p>`;
    if (!t.models.length) h += '<p class="bad">This tone has no models of the chosen kind.</p>';
    else h += `<div class="row"><span>Folder on the card</span><input type="text" id="tfolder" value="${esc(t.suggested_folder)}" style="width:220px"><button class="primary" id="tgo">Send ticked models to the card</button></div>` +
      '<div class="scroll"><table><tr><th class="tick"></th><th>Model</th><th class="size">Size</th><th class="name">Name on the MPC</th></tr>' +
      t.models.map((m, i) => `<tr><td class="tick"><input type="checkbox" class="tm" data-i="${i}" ${t.models.length <= 6 ? 'checked' : ''}></td><td>${esc(m.name)}</td><td><span class="pill">${esc(m.architecture === '2' ? 'A2' : m.architecture === '1' ? 'A1' : m.architecture)} ${esc(m.size || '')}</span></td><td class="name"><input type="text" class="tn" data-i="${i}" value="${esc(m.suggested_name)}" maxlength="24"></td></tr>`).join('') + '</table></div>';
    $('tone').innerHTML = h;
    if ($('tgo')) $('tgo').onclick = async () => {
      const models = [...document.querySelectorAll('.tm')].filter(c => c.checked).map(c => ({ id: t.models[c.dataset.i].id, name: document.querySelector(`.tn[data-i="${c.dataset.i}"]`).value }));
      if (!models.length) return toast('Tick at least one model', true);
      $('tgo').disabled = true;
      try { const r = await api('/api/t3k/install', { tone_id: t.id, arch, folder: $('tfolder').value, models }); toast(r.installed.length + ' profile(s) on the card'); refresh(); } catch (e) { toast(e.message, true); }
      $('tgo').disabled = false;
    };
  } catch (e) { toast(e.message, true); }
}
function renderGh() {
  const q = $('ghq').value.toLowerCase().split(/\s+/).filter(Boolean);
  const rows = gh.filter(f => q.every(w => f.path.toLowerCase().includes(w))).slice(0, 300);
  $('gh').innerHTML = '<tr><th class="tick"></th><th>Capture</th><th class="name">Name on the MPC</th></tr>' + rows.map(f =>
    `<tr><td class="tick"><input type="checkbox" class="gc" data-p="${esc(f.path)}" ${picked.has(f.path) ? 'checked' : ''}></td><td class="small">${esc(f.path.replace(/\.nam$/i, ''))}</td><td class="name"><input type="text" class="gn" data-p="${esc(f.path)}" value="${esc(picked.get(f.path) ?? f.suggested_name)}" maxlength="24"></td></tr>`).join('');
  document.querySelectorAll('.gc').forEach(c => c.onchange = () => { const n = document.querySelector(`.gn[data-p="${CSS.escape(c.dataset.p)}"]`).value; c.checked ? picked.set(c.dataset.p, n) : picked.delete(c.dataset.p); $('ghgo').disabled = !picked.size; $('ghgo').textContent = picked.size ? `Send ${picked.size} to card` : 'Send to card'; });
  document.querySelectorAll('.gn').forEach(i => i.oninput = () => { if (picked.has(i.dataset.p)) picked.set(i.dataset.p, i.value); });
}
function renderLib() {
  const c = status.card;
  if (!c) { $('lib').innerHTML = '<p class="muted">No card.</p>'; return; }
  if (!c.profiles.length) { $('lib').innerHTML = '<p class="muted">No profiles in "NAM Profiles" yet.</p>'; return; }
  $('lib').innerHTML = '<table><tr><th>Folder (Folder knob)</th><th>Profile (Model knob)</th><th class="size">Size</th><th class="size"></th></tr>' + c.profiles.map(f => f.files.map((x, i) =>
    `<tr><td>${i ? '' : '<b>' + esc(f.folder || 'Profiles') + '</b>'}</td><td>${i + 1} ${esc(x.name.replace(/\.nam$/i, ''))}</td><td class="num">${x.bytes == null ? '' : Math.round(x.bytes / 1024) + ' KB'}</td><td><button class="del small" data-f="${esc(f.folder)}" data-n="${esc(x.name)}">Remove</button></td></tr>`).join('')).join('') + '</table>';
  document.querySelectorAll('.del').forEach(b => b.onclick = async () => { if (!confirm('Remove "' + b.dataset.n + '" from the card?')) return; try { await api('/api/delete', { folder: b.dataset.f, file: b.dataset.n }); refresh(); } catch (e) { toast(e.message, true); } });
}
async function upload(files) {
  let n = 0;
  for (const f of files) { if (!/\.nam$/i.test(f.name)) continue;
    try { await api('/api/upload?folder=' + encodeURIComponent($('upfolder').value) + '&name=' + encodeURIComponent(f.name), await f.arrayBuffer(), true); n++; } catch (e) { toast(f.name + ': ' + e.message, true); } }
  if (n) { toast(n + ' profile(s) on the card'); refresh(); }
}
$('ghq').oninput = renderGh;
$('ghgo').onclick = async () => {
  $('ghgo').disabled = true;
  try { const r = await api('/api/github/install', { folder: $('ghfolder').value, files: [...picked].map(([path, name]) => ({ path, name })) }); toast(r.installed.length + ' profile(s) on the card'); picked.clear(); renderGh(); refresh(); } catch (e) { toast(e.message, true); }
  $('ghgo').textContent = 'Send to card';
};
const drop = $('drop');
drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); }; drop.ondragleave = () => drop.classList.remove('over');
drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); upload(e.dataTransfer.files); };
$('pick').onchange = e => upload(e.target.files);
(async () => {
  try { await refresh(); } catch (e) { toast(e.message, true); }
  try { gh = (await api('/api/github/list')).files; renderGh(); } catch (e) { $('gh').innerHTML = '<tr><td class="bad">' + esc(e.message) + '</td></tr>'; }
  const h = new URLSearchParams(location.hash.slice(1));
  if (h.get('error')) toast('TONE3000: ' + h.get('error'), true);
  if (h.get('tone')) { showTone(h.get('tone'), h.get('arch') || '2'); }
  if (location.hash) history.replaceState(null, '', '/');
})();
</script></body></html>
"""


def main():
    ap = argparse.ArgumentParser(description="Browse NAM profiles online and put them on the MPC's card.")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--card", help="use this folder as the card (it gets 'NAM Profiles/' and 'VST2 Plugins/')")
    ap.add_argument("--lan", action="store_true", help="listen on the local network too (for using a phone as the screen)")
    ap.add_argument("--mpc", metavar="HOST", help="address of the MPC on the USB network link, if it is not mpc-sample.local")
    args = ap.parse_args()
    hosts = ([args.mpc] if args.mpc else []) + (os.environ.get("NAM_LOADER_LINK_HOSTS", "").split(",") if os.environ.get("NAM_LOADER_LINK_HOSTS") is not None else LINK_HOSTS)
    threading.Thread(target=link_probe_forever, args=([h for h in hosts if h],), daemon=True).start()
    if args.card:
        os.makedirs(args.card, exist_ok=True)
        STATE["card_override"] = os.path.abspath(args.card)
    server = http.server.ThreadingHTTPServer(("0.0.0.0" if args.lan else "127.0.0.1", args.port), Handler)
    print("NAM Card Loader: open http://localhost:%d   (Ctrl+C to stop)" % args.port)
    card = current_card()
    print("MPC: %s" % (card["path"] if card else "not found yet (load NAM Player on the MPC Sample, or put it in SD Card Access mode)"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
