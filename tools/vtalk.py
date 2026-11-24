#!/usr/bin/env python3
"""vtalk - talk to Vector's mind while it runs as a service.

  vtalk hello Vector            conversation: he hears it like speech, remembers it, and answers
  vtalk /status                 maintenance: a command for the brain (not remembered, not sent to his mind)
  vtalk                         interactive: type lines; "/..." lines are commands, the rest is conversation

Needs the brain running with --input-fifo <data>/talk.fifo. Data dir: --data DIR, $MBOT_DATA, or the only
folder in ~/.local/share/mbot/.
"""
import glob, os, sys, time

def data_dir(argv):
    if len(argv) > 2 and argv[1] == "--data":
        d = argv[2]; del argv[1:3]; return d
    if os.environ.get("MBOT_DATA"):
        return os.environ["MBOT_DATA"]
    dirs = [d for d in glob.glob(os.path.expanduser("~/.local/share/mbot/*")) if os.path.isdir(d)]
    if len(dirs) == 1:
        return dirs[0]
    sys.exit("vtalk: can't tell which robot - use --data DIR or set MBOT_DATA" if dirs else
             "vtalk: no mind folder in ~/.local/share/mbot/")

D = data_dir(sys.argv)
FIFO, REPLY, LOG = (os.path.join(D, f) for f in ("talk.fifo", "console.reply", "brain_service.log"))

def send(line):
    try:
        fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)
    except FileNotFoundError:
        sys.exit("vtalk: no talk.fifo - is the brain running with --input-fifo?")
    except OSError:
        sys.exit("vtalk: the brain isn't listening right now (not running, or still waiting for the robot)")
    os.write(fd, (line.strip() + "\n").encode())
    os.close(fd)

def command(line):
    before = os.stat(REPLY).st_mtime if os.path.exists(REPLY) else 0
    send(line)
    deadline = time.time() + (400 if line.split()[0] in ("/reflect",) else 20)
    while time.time() < deadline:
        if os.path.exists(REPLY) and os.stat(REPLY).st_mtime != before:
            time.sleep(0.05)
            print(open(REPLY).read().rstrip())
            return
        time.sleep(0.15)
    print("(no answer from the brain - is it busy, or waiting for the robot?)")

def robot_online():
    try:
        import json
        st = json.load(open(os.path.join(D, "status.json")))
        return st.get("robot_online", True) and time.time() - st.get("updated", 0) < 30
    except Exception:
        return True

def converse(line):
    if not robot_online():
        send(line)
        print("(he's offline right now - asleep, flat, rebooting or off Wi-Fi. He'll hear this when he's back.)")
        return
    log = open(LOG, "r", errors="replace") if os.path.exists(LOG) else None
    if log:
        log.seek(0, 2)
    send(line)
    if not log:
        return
    deadline, said = time.time() + 25, False
    while time.time() < deadline:
        l = log.readline()
        if not l:
            if said:            # he finished speaking
                return
            time.sleep(0.15)
            continue
        if l.startswith("[say] "):
            print("Vector:", l[6:].strip())
            said, deadline = True, time.time() + 1.5
        elif "keeps the thought to itself" in l:
            print("(he thought about it but said nothing)")
            return
    if not said:
        print("(no spoken reply)")

def handle(line):
    line = line.strip()
    if not line:
        return
    (command if line.startswith("/") else converse)(line)

if len(sys.argv) > 1:
    handle(" ".join(sys.argv[1:]))
else:
    try:
        import readline  # noqa: F401  (line editing + history)
    except ImportError:
        pass
    print("Talking to Vector. Plain lines = conversation; /help = maintenance commands; Ctrl-D to quit.")
    while True:
        try:
            handle(input("you> "))
        except (EOFError, KeyboardInterrupt):
            print()
            break
