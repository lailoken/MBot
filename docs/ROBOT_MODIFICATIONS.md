# Vector robot modifications for MBot

Everything that was done **to the robot itself** (firmware, access, files, services) so MBot can see,
hear all the time, and control him. Each change says what it is, why it exists, how to check it, and how
to **undo** it. PC-side pieces are summarised at the end.

> Commands assume the robot's IP in `$ROBOT` and the SSH helper `~/.config/mbot/vssh` (see §3). **No secrets are recorded here**: keys, tokens and
> GUIDs live only in the files named.

---

## 1. Firmware this was built and tested on

| Item | Value |
|---|---|
| Robot | Anki/DDL **Vector** (1st gen), e.g. name `Vector-XXXX`, serial `<serial>` |
| SoC / board | Qualcomm APQ8009 / MSM8909 (`ro.product.board=msm8909`), 4× Cortex-A7, armv7 hard-float, ~474 MB RAM |
| Firmware | **WireOS 3.0.2.36d** (`mbot info` → `os 3.0.2.36d`), Vector compat version **3.0.2.0**, revision `ebf38c28` |
| OS build | `msm-perf 202610250004` (OpenEmbedded), kernel `4.9.277-wrynose-perf` (built 2026-10-25) |
| Boot | dev/unlocked build (`anki.dev` on the kernel command line), slot `_b`, root FS `/dev/mmcblk0p25` (**read-only**) |
| Userland | glibc **2.43** (`/lib/ld-linux-armhf.so.3`), systemd, BusyBox (`/bin/sh`), OpenSSH (port 22, socket-activated) |
| Cloud | community server `vicapi.pvic.xyz` (set in `/anki/data/assets/cozmo_resources/config/server_config.json`). MBot does **not** need a cloud account |

**What the hacks depend on.** The microphone shim (§7) relies on vic-anim reading the robot's raw mic
packets with `recv()` in a specific format (§7.1). Other firmware builds may differ. Before installing
the shim on a different build, re-check the packet format (§7.1) and run the shim self-test (§7.4).
mictap (§6) needs `/dev/socket/mic_sock` with the vector-cloud CLAD protocol.

---

## 2. Getting a Vector onto rooted WireOS

Starting point: a Vector on stock (Anki/DDL) firmware.

1. **Recovery mode.** On the charger, hold the back button **~15 s** until the **two back lights turn dark
   blue**. Only in this mode does the web tool flash firmware.
2. **Flash WireOS.** In Chrome (Web Bluetooth) open **websetup.skittle.dev** → **Custom firmware** stack →
   pair with your Vector (`Vector-XXXX`) → enter the PIN shown on his face → Wi-Fi SSID + password
   (2.4 GHz is safest) → **choose the WireOS image** → flash (~7 min, keep him on the charger).
   ⚠️ Pick only the WireOS image. The images offered by the *Prod* stack are locked stock firmware and
   remove root access.
3. **Wi-Fi again if needed.** Flashing wipes Wi-Fi. If he shows `u.skittle.dev` on his face, run the
   onboarding again (pair → PIN → Wi-Fi). While he's booted normally (not in recovery mode) this doesn't
   flash anything.
4. **Root SSH** with the **community global key** (comment `victor-dev@anki`, installed by WireOS in
   `/etc/ssh/authorized_keys`), see §3.
5. **SDK access for MBot without a cloud account:** a locally forged token, see §4.

**Finding his IP:** double-click the back button, then raise and lower the lift. The debug screen shows
Wi-Fi state and IP.

---

## 3. Root SSH from a modern Linux PC

The robot's OpenSSH only offers `ssh-rsa` (SHA-1), which Fedora/Nobara refuse by default.
`~/.config/mbot/vssh` wraps the workaround:

```sh
OPENSSL_CONF=~/.config/mbot/ssl_legacy.cnf \
ssh -i ~/.config/mbot/vector_ssh_key -o IdentitiesOnly=yes \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ConnectTimeout=8 root@$ROBOT "$@"
```

`ssl_legacy.cnf` enables OpenSSL's legacy provider and `rh-allow-sha1-signatures = yes`. Usage:
`vssh $ROBOT 'command'` (**the IP is the first argument**). The private key file is the community global
key from the unlock site. Keep it private anyway.

**Reversible:** PC-side only. Delete `~/.config/mbot/{vssh,ssl_legacy.cnf,vector_ssh_key}`.

---

## 4. SDK (gRPC) access for MBot, with no cloud account

- On WireOS the SDK gateway is built into **`vic-cloud`**, TLS on **:443** (not 9090). Its certificate is
  pinned on the PC in `~/.anki_vector/Vector-XXXX-<serial>.cert`, referenced from
  `~/.anki_vector/sdk_config.ini` (`cert`, `ip`, `name`, `guid`), the same file format the Python SDK
  and wire-pod use.
- The gateway authorises any client whose token hash is in **`/data/vic-gateway/token-hashes.json`**.
  MBot's token was **forged locally and merged in** by `~/.config/mbot/forge-token.sh`, using wire-pod's
  scheme: `stored = base64( sha256(token16 ‖ salt16) ‖ salt16 )`, `GUID = base64(token16)`. The script
  backs the file up first (`token-hashes.json.bak`) and **reboots** the robot to load it.
- ⚠️ **Never `systemctl restart vic-cloud`.** Its :443 listener depends on a socket handoff that is set up
  only at boot. Restarting it leaves :443 dead until a reboot.

**Reversible:**
`vssh $ROBOT 'cp /data/vic-gateway/token-hashes.json.bak /data/vic-gateway/token-hashes.json && reboot'`
(removes MBot's token; other clients keep theirs).

---

## 5. Read-only root filesystem: how changes under `/etc` were made

`/` is read-only by design (`/data` is the writable partition). For each change under `/etc`:

```sh
vssh $ROBOT 'mount -o remount,rw / && <edit files> ; sync; mount -o remount,ro /'
```

Always remount read-only again. A firmware update or reflash replaces `/` and **silently removes every
`/etc` change below**. Files under `/data` survive updates but not a factory reset.

---

## 6. Mod: `mictap`, the robot-side microphone streamer

**What it is.** A small static Go program, `/data/mictap`, that streams Vector's microphones to the PC as
16 kHz / 16-bit mono PCM on **TCP :9099**. The robot listens and the PC connects in, which avoids the PC's
firewall. It has two sources:
1. **Raw, constant** (preferred): packets from the mic shim (§7) on the unix datagram socket
   `/dev/socket/mbot_rawmic`. It sums the 4 mics, runs two 1-pole ~150 Hz high-pass stages (removes each
   mic's DC offset and the fan/body rumble; about 74% of measured noise energy is below 100 Hz), and
   resamples 15625 → 16000 Hz (exactly 125/128). Uses ~4% of one CPU core.
2. **Gated, fallback**: the firmware's `/dev/socket/mic_sock` (vector-cloud CLAD protocol, registered as
   client `cp_mbot`). Audio only arrives after "Hey Vector". Used only while no raw packets are flowing.

Only one client at a time; a new connection replaces the old one.

**Direction of sound (TCP :9098).** mictap also keeps the last ~0.5 s of raw 4-channel packets in a ring
buffer. MBot connects to :9098 and, only while it hears someone speaking, sends `S` (mictap replies with
the ring buffer, then live packets, unchanged: 1293 bytes each) and `E` to stop. The PC works out the
direction from the time differences between the mics. Nothing is sent while nobody speaks.

**Install / update:** `robot/install.sh <robot-ip>` builds, uploads (checksum-verified, old binary kept as
`.prev`), self-tests the shim on the robot before using it, writes the units below if missing, and asks
before rebooting. `robot/install.sh <ip> --check` only reports.

**Files**
| Path | Purpose |
|---|---|
| `/data/mictap` | current binary |
| `/data/mictap.gated` | original wake-word-only binary (backup) |
| `/etc/systemd/system/mictap.service` | unit (below) |
| `/etc/systemd/system/mictap.service.bak` | the first, simpler version of the unit |
| `/etc/systemd/system/multi-user.target.wants/mictap.service` | enable symlink |

```ini
[Unit]
Description=MBot microphone tap (streams Vector mic to the PC)
After=vic-cloud.service vic-robot.service vic-anim.service
Wants=vic-cloud.service vic-robot.service
[Service]
ExecStartPre=/bin/sh -c "while [ ! -S /dev/socket/mic_sock ]; do sleep 1; done"
ExecStartPre=/bin/sleep 8
ExecStart=/data/mictap
Restart=always
RestartSec=5
[Install]
WantedBy=multi-user.target
```

The two `ExecStartPre` lines fix a "stale after reboot" state: registering on `mic_sock` too early let
mictap accept connections but never deliver audio.

**Build:** `robot/build.sh` (podman `golang:1.22` container). Source `robot/mictap/main.go`, compiled
inside `third_party/vector-cloud` (it uses vector-cloud's internal `ipc` and `clad/cloud` packages).
**`-tags vicos` is required**; without it the socket paths point to `/tmp` instead of `/dev/socket`.

**Check:** `vssh $ROBOT 'systemctl is-active mictap; journalctl -u mictap -b --no-pager | tail'` should
show `[raw] raw mic stream flowing - constant listening`.

**Undo:**
```sh
vssh $ROBOT 'systemctl disable --now mictap.service; mount -o remount,rw / &&
  rm -f /etc/systemd/system/mictap.service /etc/systemd/system/mictap.service.bak \
        /etc/systemd/system/multi-user.target.wants/mictap.service;
  sync; mount -o remount,ro /; systemctl daemon-reload;
  rm -f /data/mictap /data/mictap.gated /dev/socket/mbot_rawmic'
```
(To go back to wake-word-only listening instead, keep the unit and `mv /data/mictap.gated /data/mictap`.)

---

## 7. Mod: raw microphone shim (constant listening + no wake reaction)

### 7.1 How Vector's microphones actually flow
The mics are **not** an ALSA capture device (`arecord -l` is empty). The body board samples **4 mics**
and sends them up the serial "spine" (`/dev/ttyHS0`) to **vic-robot**. vic-robot forwards them to
**vic-anim** as datagrams on `/dev/socket/_anim_robot_client_0`, which vic-anim reads with `recv()`.
vic-anim runs the wake-word engine, and only after "Hey Vector" does it pass audio on to `mic_sock`.

Mic packet (identified with `strace` on vic-anim, ~97.7 per second):

| Bytes | Content |
|---|---|
| 0 | CLAD tag **`0xf2`** (MicData) |
| 1–1280 | 640 × int16 LE, **planar**: ch0[160] ch1[160] ch2[160] ch3[160] at **15 625 Hz** (each mic has its own DC offset, around −1500…−1900) |
| 1281–1292 | u32 timestamp ms, u32 status flags, f32 head angle |

Total **1293 bytes**. The other robot→anim packets (e.g. 219-byte `0xf0` state) are left alone.

### 7.2 What the shim does
`/data/mbot/libmbotmic.so` (source `robot/shim/mbotmic.c`, ~60 lines) is **LD_PRELOAD**ed into
vic-anim. It hooks `recv()`. For every 1293-byte `0xf2` packet it:
1. **copies** the packet to `/dev/socket/mbot_rawmic` (non-blocking; if nobody is listening it backs
   off ~1 s; it never blocks or allocates), so mictap gets constant real audio;
2. while **`/data/mbot/mic_silence`** exists (checked about once a second, live toggle), **zeroes the
   samples** before vic-anim sees them. The firmware then never hears "Hey Vector", so it never does
   its **wake reaction** (the lift/head jump). That jump's sudden load was a brownout trigger on his
   weak battery. MBot's own brain still hears everything and responds to his name.

No Anki binary is modified. The shim is injected with a systemd drop-in:

```ini
# /etc/systemd/system/vic-anim.service.d/mbot-mic.conf
[Service]
Environment=LD_PRELOAD=/data/mbot/libmbotmic.so
```

### 7.3 Effects to know about
- **Silence ON** (current): no firmware "Hey Vector" (wake word, voice commands, and the wake jump);
  "react to loud sound" and turning toward voices stop too. vic-anim accepts all-zero mic data with no
  fault (verified for minutes).
- The firmware's wake word also used to **wake him from sleep**. MBot now handles that itself: when he is
  dozing (`ROBOT_STATUS_CALM_POWER_MODE`) and hears his name, it takes control at OVERRIDE priority.
- **Silence OFF** (`rm /data/mbot/mic_silence`): the firmware hears normally again and MBot still gets
  constant audio.

### 7.4 Build and self-test
`robot/build.sh` builds `libmbotmic.so` and `shimtest` (Debian `gcc-arm-linux-gnueabihf` in the
container). **Test before touching vic-anim:**
```sh
vssh $ROBOT 'cd /data/mbot; LD_PRELOAD=./libmbotmic.so /bin/true && echo loads;
  rm -f mic_silence; LD_PRELOAD=./libmbotmic.so ./shimtest     # fwd_ok=1 samples_intact=1
  touch mic_silence; LD_PRELOAD=./libmbotmic.so ./shimtest     # fwd_ok=1 samples_zeroed=1'
```
Then install the drop-in (§5 for the rw remount) and **reboot** to load it. Don't restart vic-anim alone:
it has `Restart=no`, and its exit handler raises a face fault code on error exits.

**Check:** `vssh $ROBOT 'grep -c libmbotmic /proc/$(pidof vic-anim)/maps'` should print a non-zero count.

### 7.5 Undo
```sh
vssh $ROBOT 'mount -o remount,rw / && rm -rf /etc/systemd/system/vic-anim.service.d/mbot-mic.conf &&
  rmdir /etc/systemd/system/vic-anim.service.d 2>/dev/null; sync; mount -o remount,ro /;
  rm -f /data/mbot/mic_silence; reboot'
```
After the reboot vic-anim is stock (verify: the `grep` above prints 0). Optionally `rm -rf /data/mbot`
afterwards, but remove the guard first (§8).

---

## 8. Mod: shim safety net (auto-rollback)

If vic-anim ever fails to start with the shim loaded, this removes the shim and reboots him to stock, with
nobody needed.

| Path | Purpose |
|---|---|
| `/etc/systemd/system/mbot-shim-guard.timer` | `OnBootSec=150`, enabled via `timers.target.wants/` |
| `/etc/systemd/system/mbot-shim-guard.service` | `Type=oneshot`, runs the script |
| `/data/mbot/shim-guard.sh` | if the drop-in exists **and** (`pidof vic-anim` fails **or** `/run/fault_code.pending` exists): remove the drop-in, reboot |
| `/data/mbot/shim-guard.log` | `boot OK: vic-anim healthy with mic shim` on every good boot |

⚠️ **`/run/fault_code` is a FIFO** that the fault display listens on. It **always exists**, so never use
it to detect a fault, and **never `cat` it**: the read blocks forever. A real fault is signalled by
`/run/fault_code.pending`.

**Undo:**
```sh
vssh $ROBOT 'mount -o remount,rw / && rm -f /etc/systemd/system/mbot-shim-guard.{timer,service} \
  /etc/systemd/system/timers.target.wants/mbot-shim-guard.timer; sync; mount -o remount,ro /;
  systemctl daemon-reload; rm -f /data/mbot/shim-guard.sh /data/mbot/shim-guard.log'
```

---

## 9. Runtime console variables (nothing persistent)

vic-engine (`:8888`) and vic-anim (`:8889`) expose console vars over local HTTP:
`curl 'http://127.0.0.1:8889/consolevarget?key=X'`, `…/consolevarset?key=X&value=Y`,
`…/consolevarlist`, `…/consolefunclist`. All values **reset on reboot**.

| Var | Default | Use |
|---|---|---|
| engine `MaxStreamingDuration_s` | 10 | how long the firmware listens after a wake word (read only) |
| engine `ListeningCooldown_sec` | 20 | firmware ignores the wake word for 20 s after an interaction (read only) |
| anim `SuppressTriggerResponse` | false | MBot sets it to true **only** in PC-mic mode (`--mic pc`) |

---

## 10. Camera ("seeing")

**No modification needed on WireOS.** Frames come over the normal SDK (`CaptureSingleImage`). While he dozes on the charger the firmware powers the
camera down and captures time out. That's normal. Face recognition is the robot's own: `mbot faces` lists
enrolled faces. Faces enrolled under an older firmware may not load after flashing (`mbot faces` shows 0):
re-enroll them (MBot does this only after checking the person matches their description).

---

## 11. Gotchas

- **Reboots**: brownout resets show `Power-off reason: Triggered from UVLO` in `dmesg`; a clean reboot
  shows `PS_HOLD`. `boot_reason=2` is a soft reset, not a crash.
- **BusyBox** `timeout`, `pidof` and `sleep` differ from GNU. `sleep` is a shell builtin (no child process).
- **`ssh` sessions that time out on the PC can leave the remote command running.** Check for stragglers
  after anything that hung.
- **Battery flags**: trust the firmware's `BATTERY_LEVEL_LOW` (with a weak cell the resting voltage lies);
  don't trust `FULL` right after a reboot.

---

## 12. Rolling everything back

**A. Back to plain WireOS (remove all MBot robot-side changes):** run, in order, the undo blocks of §8
(guard), §7.5 (shim, which reboots), §6 (mictap), then §4 (token). Afterwards:
`vssh $ROBOT 'ls /data/mbot /data/mictap* /etc/systemd/system | grep -iE "mbot|mictap"'` shows nothing,
and `grep -c libmbotmic /proc/$(pidof vic-anim)/maps` prints 0.

**B. Back to stock firmware (re-lock):** recovery mode (§2.1) → websetup.skittle.dev → **Prod** stack →
pick a Prod image → flash. This removes WireOS, root SSH, and everything above, and he then needs DDL's
app/cloud again. It restores *a* stock build, not necessarily the exact one he had before: anything unique
to his previous firmware (e.g. a per-robot SSH key) can't be brought back.

---

## 13. PC-side pieces (for completeness)

| Piece | Where | Undo |
|---|---|---|
| Brain service | `~/.config/systemd/user/mbot-brain.service` (`--mic tap --whisper models/ggml-small.en.bin --fast-model qwen2.5:3b --input-fifo …/talk.fifo`) | `systemctl --user disable --now mbot-brain` |
| Live mind page | `mbot-webviz.service` → `tools/mindview.py` + `web/mind_monitor.html`, http://localhost:8700 (localhost only) | `systemctl --user disable --now mbot-webviz` |
| Local models | `ollama.service` (user): `minicpm-v` (eyes), `qwen2.5:3b` (dreaming) | `systemctl --user disable --now ollama` |
| Talk to him | `~/.local/bin/vtalk "…"` (named pipe into the brain); `vtalk battery fixed` clears battery safe-mode | delete the script |
| Robot access | `~/.config/mbot/{vssh,ssl_legacy.cnf,vector_ssh_key,forge-token.sh}`, `~/.anki_vector/` | delete |
| Mind data | `~/.local/share/mbot/<serial>/` (memories, people, self, logs, `history/` snapshots) | back it up before deleting. It's who he is |
