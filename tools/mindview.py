#!/usr/bin/env python3
"""mindview - live web view of Vector's mind (localhost only).

Serves the mind monitor page and a Server-Sent-Events stream at /events, so the page updates the moment
something happens - no polling, no reloads:
  * "ev"    - each thing that goes through his mind (heard, thought, say, act, eyes, memory, dream, ...),
              parsed from the tagged lines the brain logs (brain_service.log)
  * "files" - a mind file changed (self.json, people.json, usage.json, status.json, brownout_safe):
              the page re-fetches just that file
On connect it first replays the recent history, then follows live.

Usage: mindview.py --data ~/.local/share/mbot/<serial> [--port 8700] [--page web/mind_monitor.html]
"""
import argparse, json, os, re, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

ap = argparse.ArgumentParser()
ap.add_argument("--data", required=True)
ap.add_argument("--port", type=int, default=8700)
ap.add_argument("--page", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web", "mind_monitor.html"))
args = ap.parse_args()
DATA = os.path.abspath(os.path.expanduser(args.data))
LOG = os.path.join(DATA, "brain_service.log")
WATCH = ["self.json", "people.json", "usage.json", "status.json", "brownout_safe", "memories.jsonl",
         "hardware.json", "battery_log.jsonl"]
MEDIA_DIRS = ["photos", "voices"]        # people's photos (+ face position) and name voice samples
SERVE = set(WATCH)                       # data files the page may fetch (nothing else from the data dir)
HISTORY = 80

# ---------------------------------------------------------------- log line -> event
PATTERNS = [
    ("thought", re.compile(r"^\[mind ([\d.]+)s · (.*?)\] \((.*)\)\s*$")),
    ("heard",   re.compile(r'^\[heard\] "(.*)" \(([\d.]+)s\)\s*$')),
    ("typed",   re.compile(r"^\[typed\] (?!listening for text)(.*)$")),
    ("sense",   re.compile(r"^\[sense\] (.*)$")),
    ("say",     re.compile(r"^\[say\] (.*)$")),
    ("act",     re.compile(r"^\[act\] (.*)$")),
    ("guard",   re.compile(r"^\[guard\] (.*)$")),
    ("eyes",    re.compile(r"^\[eyes ([\d.]+)s[^\]]*\] (.*)$")),
    ("memory",  re.compile(r"^\[memory\+\] (.*)$")),
    ("insight", re.compile(r"^\[insight\] (.*)$")),
    ("reflect", re.compile(r"^\[reflect\] (.*)$")),
    ("dream",   re.compile(r"^\[dream\] (.*)$")),
    ("body",    re.compile(r"^\[body\] (.*)$")),
    ("people",  re.compile(r"^\[(?:people|faces)\] (.*)$")),
    ("care",    re.compile(r"^\[(?:self-care|battery|battery-health)\] (.*)$")),
    ("cost",    re.compile(r"^\s*\$\$\$ CLAUDE API CALL \$\$\$\s+(\S+)\s+cost \$([\d.]+).*today \$([\d.]+) of \$([\d.]+)")),
    ("wake",    re.compile(r"^===== brain started (.*)$")),
]
TRAILING_JUNK = re.compile(r"[\s})\];*.]*[})\];][\s})\];*.;]*$")

def parse(line):
    line = line.rstrip("\n")
    for kind, rx in PATTERNS:
        m = rx.match(line)
        if not m:
            continue
        g = m.groups()
        if kind == "thought":
            text = TRAILING_JUNK.sub("", g[2]).strip()
            text = re.sub(r"(\s*;\s*){2,}", " ", text)          # model glitch: "; ; ; ;"
            return {"type": kind, "text": text, "by": g[1], "secs": float(g[0])}
        if kind == "heard":
            return {"type": kind, "text": g[0], "secs": float(g[1])}
        if kind == "eyes":
            return {"type": kind, "text": g[1]}
        if kind == "cost":
            return {"type": kind, "model": g[0], "cost": float(g[1]), "today": float(g[2]), "budget": float(g[3])}
        if kind == "wake":
            return {"type": kind, "text": "mind (re)started"}
        if kind == "act" and g[0].strip() in ("anim 0", "head 0"):
            return None
        return {"type": kind, "text": g[0].strip()}
    return None

# ---------------------------------------------------------------- hub: history + live fan-out
class Hub:
    def __init__(self):
        self.lock = threading.Condition()
        self.events = []        # (seq, sse_event, json_payload)
        self.seq = 0

    def publish(self, kind, payload):
        with self.lock:
            self.seq += 1
            self.events.append((self.seq, kind, json.dumps(payload)))
            if len(self.events) > 500:
                self.events = self.events[-400:]
            self.lock.notify_all()

    def since(self, seq, timeout):
        with self.lock:
            if not any(s > seq for s, _, _ in self.events):
                self.lock.wait(timeout)
            return [e for e in self.events if e[0] > seq]

hub = Hub()

def follow_log():
    pos, ino = None, None
    while True:
        try:
            st = os.stat(LOG)
            if ino != st.st_ino or (pos is not None and st.st_size < pos):
                pos, ino = None, st.st_ino            # rotated / truncated
            with open(LOG, "r", errors="replace") as f:
                if pos is None:                       # first look: replay recent history only
                    f.seek(max(0, st.st_size - 256 * 1024))
                    if f.tell():
                        f.readline()
                    hist = [e for e in (parse(l) for l in f) if e]
                    for e in hist[-HISTORY:]:
                        e["t"] = None                 # log lines carry no clock time
                        e["history"] = True
                        hub.publish("ev", e)
                    pos = f.tell()
                else:
                    f.seek(pos)
                    for line in f:
                        if not line.endswith("\n"):
                            break                     # partial line: read it next time
                        pos += len(line.encode("utf-8", "replace"))
                        e = parse(line)
                        if e:
                            e["t"] = time.time()
                            hub.publish("ev", e)
        except FileNotFoundError:
            pass
        except Exception as ex:                       # never die; just retry
            print("follow_log:", ex, flush=True)
        time.sleep(0.25)

def media_index():
    out = {"photos": {}, "voices": []}
    pdir, vdir = os.path.join(DATA, "photos"), os.path.join(DATA, "voices")
    for f in sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []:
        name, ext = os.path.splitext(f)
        if ext.lower() in (".jpg", ".jpeg", ".png", ".webp"):
            meta = {}
            try:
                with open(os.path.join(pdir, name + ".json")) as m:
                    meta = json.load(m)
            except Exception:
                pass
            meta["file"] = "photos/" + f
            meta.setdefault("taken", int(os.stat(os.path.join(pdir, f)).st_mtime))
            out["photos"][name] = meta
    for f in sorted(os.listdir(vdir)) if os.path.isdir(vdir) else []:
        if f.lower().endswith(".wav"):
            out["voices"].append(os.path.splitext(f)[0])
    return out

def watch_files():
    seen = {}
    while True:
        changed = {}
        for d in MEDIA_DIRS:
            p = os.path.join(DATA, d)
            try:
                mt = max([os.stat(p).st_mtime] + [os.stat(os.path.join(p, f)).st_mtime for f in os.listdir(p)])
            except FileNotFoundError:
                mt = 0
            if seen.get(d) != mt:
                seen[d] = mt
                changed["media.json"] = mt
        for name in WATCH:
            try:
                mt = os.stat(os.path.join(DATA, name)).st_mtime
            except FileNotFoundError:
                mt = 0
            if seen.get(name) != mt:
                seen[name] = mt
                changed[name] = mt
        if changed:
            hub.publish("files", changed)
        time.sleep(0.5)

# ---------------------------------------------------------------- http
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send_file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                body = f.read()
        except FileNotFoundError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/mind_monitor.html", "/index.html"):
            return self.send_file(os.path.abspath(args.page), "text/html; charset=utf-8")
        if path in ("/memories.html", "/hardware.html"):  # other pages live next to the main one
            return self.send_file(os.path.join(os.path.dirname(os.path.abspath(args.page)), path.lstrip("/")),
                                  "text/html; charset=utf-8")
        if path == "/events":
            return self.events()
        if path == "/media.json":
            body = json.dumps(media_index()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        name = path.lstrip("/")
        if name in SERVE:
            return self.send_file(os.path.join(DATA, name), "application/json" if name.endswith(".json") else "text/plain; charset=utf-8")
        if (name.startswith("photos/") or name.startswith("voices/")) and ".." not in name and name.count("/") == 1:
            ext = name.rsplit(".", 1)[-1].lower()
            ctype = {"jpg": "image/jpeg", "jpeg": "image/jpeg", "png": "image/png", "webp": "image/webp",
                     "wav": "audio/wav"}.get(ext)
            if ctype:
                return self.send_file(os.path.join(DATA, name), ctype)
        self.send_error(404)

    def events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        seq = 0
        try:
            # full file list first, so the page loads everything once
            self.wfile.write(b"event: files\ndata: " + json.dumps({n: 1 for n in WATCH + ["media.json"]}).encode() + b"\n\n")
            while True:
                batch = hub.since(seq, 15)
                if not batch:
                    self.wfile.write(b": keep-alive\n\n")
                for s, kind, payload in batch:
                    seq = s
                    if kind == "files" and s == 0:
                        continue
                    self.wfile.write(f"event: {kind}\ndata: {payload}\n\n".encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            return

threading.Thread(target=follow_log, daemon=True).start()
threading.Thread(target=watch_files, daemon=True).start()
srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
srv.daemon_threads = True
print(f"mindview: http://localhost:{args.port}/  (data {DATA})", flush=True)
srv.serve_forever()
