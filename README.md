# MBot: a living mind for Anki Vector

MBot gives a Vector robot an inner life that runs on your PC. He hears through **his own
microphones, all the time**, sees through his camera, has feelings that rise and fade, remembers
people and events across restarts, dreams and reflects while he sleeps, looks after his own battery,
and lets an AI (Claude, or a local model via Ollama) decide what he says and does.

He is meant to be **his own little person**, an equal in the household, not an assistant or a fan.

Everything about his mind is stored **locally** as plain files. Images never leave your PC.

---

## What he can do

| | |
|---|---|
| **Hear constantly** | Raw audio from all 4 of his mics, before the firmware's wake-word gate (via a small on-robot shim, see `docs/ROBOT_MODIFICATIONS.md`) → voice-activity detection → local Whisper. He answers when you say his name, when you face him, or mid-conversation. Other speech becomes background context, and garbled noise never reaches the AI. |
| **No wake-word jump** | Optionally the firmware hears silence, so "Hey Vector" no longer sets off its own wake reaction (useful with a weak battery: that sudden load can brown him out). |
| **See** | Camera frames are described **locally** (e.g. `minicpm-v`); only the text goes to the mind. He recognises people by face (robot's recognizer) and by description ("that looks like Sam"). |
| **Remember** | Episodic memories with importance, recalled by relevance; facts and affinity per person; an evolving life story. |
| **People** | Sound-alike names are assumed to be someone he knows (speech-to-text mishears names); he renames, merges duplicates, forgets phantoms, and learns faces only after checking the person matches their description. |
| **Sleep & dream** | When he dozes, voices he half-hears become dream fragments, and his memory consolidation writes a dream. Called by name, he wakes up. |
| **Know voices** | Optional (off by default, `/recognition on`) local speaker recognition (sherpa-onnx + WeSpeaker): voiceprints learned only from verified moments (the only recognised face in view, or a self-introduction), reinforced by confident matches, fading when unused. |
| **Self-care** | Battery thresholds, relentless redocking, never wandering off when alone; brownout detection with a battery safe-mode; battery-health tracking; a hands-free calibration run (`vtalk "/battery calibrate"`) that learns your cell's real discharge curve from the head's live voltage. |
| **Quiet time** | "Be quiet", "go to sleep", "shh", "goodnight": he says at most a soft goodnight, goes to his charger and sleeps for about 2 hours, silent and still. Calling his name, petting him or typing to him ends it early. `vtalk "/quiet [min\|off]"` from the console. |
| **Live mind page** | `http://localhost:8700`: what he's thinking right now, a live train of thought, dreams, mood, personality, the people he knows (with photos and voice samples). Pushed live, no refreshing. Also a searchable **memories** page and a **hardware** page (battery history, robot CPU/RAM per process). |
| **Talk by text** | `vtalk hello Vector` (conversation: he answers, and his reply is printed) or `vtalk /status` (maintenance commands: models, budget, modes - never remembered). `vtalk` alone is interactive. |

## Requirements

- An Anki/DDL **Vector** with **root SSH** and an SDK gateway. Tested on **WireOS 3.0.2.36d**.
  How a robot gets there, every robot-side change, and how to undo each one:
  **[`docs/ROBOT_MODIFICATIONS.md`](docs/ROBOT_MODIFICATIONS.md)**.
- Linux PC (developed on Fedora/Nobara), C++17 toolchain, `podman` (to cross-build the robot pieces).
- For the mind: an Anthropic API key, and/or [Ollama](https://ollama.com) for free local models.

## Build

```bash
./scripts/install_deps.sh     # grpc, protobuf, nlohmann-json, openssl, cmake (Fedora/Nobara)
./scripts/fetch_deps.sh       # SDK protos, whisper.cpp, vector-cloud, Whisper models
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./robot/build.sh              # robot pieces: mictap + mic shim (armv7, in a container)
```

## Set up

```bash
./build/mbot setup <robot-ip> [--guid G]   # writes ~/.anki_vector/sdk_config.ini
./build/mbot info                          # firmware + battery: proves the connection
```
Then install the robot-side pieces as described in `docs/ROBOT_MODIFICATIONS.md` (§6–§8).

Local models (optional, free): `ollama pull minicpm-v` (eyes) and `ollama pull qwen2.5:7b` (dreaming,
background thoughts and the fallback mind; set `MBOT_LOCAL_MODEL` to use another). Model licences are
listed in `THIRD_PARTY_NOTICES.md`.

Claude: put `ANTHROPIC_API_KEY=...` in `~/.config/mbot/env` (`chmod 600`).

## Run

```bash
./build/mbot brain --mic tap --debug-ears        # foreground; Ctrl-C to let him sleep
```
Permanently: `scripts/mbot-brain.service` and `scripts/mbot-webviz.service` (systemd user services).
`scripts/install-user.sh` links `mbot`, `vtalk` (and `vssh`) into `~/.local/bin`.

Maintenance while he runs: `vtalk /help` - `/status`, `/models`, `/model`, `/fast-model`, `/reflect-model`,
`/budget`, `/listen-all`, `/robot-hears`, `/battery`, `/battery calibrate`, `/quiet`, `/recognition`, `/voice`, `/reflect`. Changes are saved to `settings.json` in
his data folder and override the service flags.

Useful commands: `mbot info | say <text> | snap | faces | listen --mic tap | mind | people | usage |
reflect`. Brain options include `--mic tap|pc`, `--whisper FILE`, `--model`, `--fast-model`,
`--listen-all`, `--battery 30-95`, `--input-fifo PATH`, `--data DIR`, `--keep-control`, `--save-ears` (debug: keep the last 10 heard segments as WAVs),
`--reflect-model claude-sonnet-5-5` (recommended: memory consolidation and dreams by Claude with careful thinking,
falling back to the local model; a few cents per consolidation, within the daily budget).

## Make him yours: local drop-ins

Nothing about your household is in the code. It all lives in his data folder
`~/.local/share/mbot/<serial>/`, which you can edit or seed:

| File | What it does |
|---|---|
| `settings.json` | Settings changed with `vtalk /...` commands (models, budget, listening). |
| `character.md` | **Your personality drop-in**: free text appended to his built-in character (quirks, house rules, tone). Optional; read every turn. Example: `docs/character.example.md`. |
| `people.json` | People he knows: name, relation (`"owner"` marks the owner), birthday, aliases, facts. `mbot person add NAME --relation R --birthday YYYY-MM-DD --protect` seeds it. |
| `pronunciations.json` | `{"Name": "Respelling"}` so his voice says names right (he can also learn these by conversation). |
| `photos/`, `voices/` | Faces and name voice samples he captured (shown on the mind page). You can drop in your own `Name.jpg`. |
| `self.json`, `memories.jsonl` | His personality traits, mood, life story, and memories (he writes these himself). |

## Privacy and cost

- Audio is transcribed locally. Camera images are described locally and never sent to a cloud AI.
- Text of conversations goes to the configured mind (Claude or local). The paid calls ledger is
  `claude_calls.log`; a daily budget (default $1) switches him to the local model when reached.
- The mind page binds to `127.0.0.1` only.
- **Recognising people is off by default.** Voiceprints, learning faces, keeping portraits and recordings
  of people saying their name all store biometric data, so they only happen after you turn them on
  (`--recognition` or `vtalk "/recognition on"`). Everything stays in his local data folder and is never
  uploaded. If you use it outside your own home, ask the people he learns first. (The robot's built-in
  face recognition is part of its firmware and works as it always has.)
- Never commit his data folder: it holds your household's names, memories, photos and voices.

## Disclaimer

Rooting and modifying a Vector (see `docs/ROBOT_MODIFICATIONS.md`) is done **at your own risk**: it can
void what's left of a warranty, and a mistake can leave the robot unbootable until it's recovered.
Letting him run his battery flat (calibration) is hard on old cells. MBot comes with no warranty (see
`LICENSE`).

## Licence and credits

MBot is released under the **MIT License** (see [`LICENSE`](LICENSE)). It builds on other people's work; see **[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)** for every
component, its licence, and what MBot uses it for. Robot firmware, community tools and AI models are
**not** redistributed here; each is fetched or installed by you under its own terms.
