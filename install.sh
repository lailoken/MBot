#!/usr/bin/env bash
# MBot, one command:  ./install.sh [--robot <ip>] [--yes]
#
# Builds MBot, connects it to your Vector, and starts it as background services. Safe to run again (it
# skips what's done). Models are NOT downloaded here: the brain fetches what it needs the first time.
#   --robot <ip>   your Vector's IP (otherwise you're asked)
#   --yes          don't ask questions (skips the optional steps that need an answer)
#   --no-services  build and set up only; don't install/start the background services
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
ROBOT=""; YES=0; SVC=1
while [ $# -gt 0 ]; do
  case "$1" in
    --robot) ROBOT=${2:-}; shift 2;;
    --yes|-y) YES=1; shift;;
    --no-services) SVC=0; shift;;
    *) sed -n '2,10p' "$0"; exit 1;;
  esac
done
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ask() { [ "$YES" = 1 ] && return 1; read -r -p "$1 [y/N] " a; [ "${a,,}" = y ]; }

step "1/6 build tools and libraries"
if command -v cmake >/dev/null && pkg-config --exists grpc++ protobuf 2>/dev/null; then echo "present"
else ./scripts/install_deps.sh; fi

step "2/6 third-party sources (pinned versions)"
NO_MODELS=1 ./scripts/fetch_deps.sh

step "3/6 build"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release | grep -E "speech-to-text|voice recognition" || true
cmake --build build -j"$(nproc)"
[ -x build/mbot ] || { echo "build failed"; exit 1; }
./scripts/install-user.sh

step "4/6 local AI (Ollama) - his eyes and the free local mind"
if command -v ollama >/dev/null; then echo "present (the brain pulls the models it needs by itself)"
else
  echo "Ollama isn't installed. Get it from https://ollama.com (Linux: curl -fsSL https://ollama.com/install.sh | sh)."
  echo "Without it he still hears, talks (with Claude) and remembers; he just can't see."
fi

step "5/6 your Vector"
if [ -z "$ROBOT" ] && [ "$YES" = 0 ]; then read -r -p "Robot IP address (blank to skip): " ROBOT; fi
SERIAL=""
if [ -n "$ROBOT" ]; then
  ./build/mbot setup "$ROBOT" || { echo "couldn't set up the SDK connection - see docs/ROBOT_MODIFICATIONS.md"; exit 1; }
  ./build/mbot info | head -5 || true
  SERIAL=$(sed -n 's/^\[\(.*\)\]$/\1/p' "$HOME/.anki_vector/sdk_config.ini" | head -1)
  if [ -x "${VSSH:-$HOME/.config/mbot/vssh}" ]; then
    if ask "Install/update the robot-side pieces (constant hearing, direction of sound) on $ROBOT?"; then
      ./robot/install.sh "$ROBOT"
    else echo "skipped (later: robot/install.sh $ROBOT)"; fi
  else
    echo "Root SSH to the robot isn't set up, so he'll only hear after \"Hey Vector\" (PC mic mode works too)."
    echo "For constant hearing see docs/ROBOT_MODIFICATIONS.md §3, then run: robot/install.sh $ROBOT"
  fi
else
  SERIAL=$(sed -n 's/^\[\(.*\)\]$/\1/p' "$HOME/.anki_vector/sdk_config.ini" 2>/dev/null | head -1 || true)
fi

step "6/6 background services"
if [ "$SVC" = 0 ] || [ -z "$SERIAL" ]; then
  [ "$SVC" = 0 ] && echo "skipped (--no-services)" || echo "skipped (no robot set up yet)"
  echo "Run him in a terminal: ./build/mbot brain --mic tap --whisper models/ggml-small.en.bin"
  exit 0
fi
DATA="$HOME/.local/share/mbot/$SERIAL"
mkdir -p "$DATA" "$HOME/.config/systemd/user" "$HOME/.config/mbot"
ENVF="$HOME/.config/mbot/env.systemd"
if [ ! -f "$ENVF" ] && [ "$YES" = 0 ]; then
  read -r -s -p "Anthropic API key for Claude (blank = local models only): " KEY; echo
  if [ -n "$KEY" ]; then umask 077; printf 'ANTHROPIC_API_KEY=%s\n' "$KEY" > "$ENVF"; unset KEY; fi
fi
MIC=tap; [ -x "${VSSH:-$HOME/.config/mbot/vssh}" ] || MIC=robot
cat > "$HOME/.config/systemd/user/mbot-brain.service" <<EOF
[Unit]
Description=MBot - AI mind for Vector
After=network-online.target ollama.service

[Service]
WorkingDirectory=$ROOT
EnvironmentFile=-$ENVF
ExecStart=$ROOT/build/mbot brain --mic $MIC --whisper $ROOT/models/ggml-small.en.bin --speaker-model $ROOT/models/speaker-campplus-en.onnx --input-fifo $DATA/talk.fifo
Restart=always
RestartSec=8
StandardInput=null
StandardOutput=append:$DATA/brain_service.log
StandardError=append:$DATA/brain_service.log

[Install]
WantedBy=default.target
EOF
cat > "$HOME/.config/systemd/user/mbot-webviz.service" <<EOF
[Unit]
Description=MBot mind monitor - live web view of Vector's mind (localhost only)

[Service]
ExecStart=/usr/bin/python3 $ROOT/tools/mindview.py --data $DATA --port 8700 --page $ROOT/web/mind_monitor.html
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
EOF
systemctl --user daemon-reload
systemctl --user enable --now mbot-brain mbot-webviz
echo
echo "He's running. Watch his mind: http://localhost:8700   Talk by text: vtalk hello"
echo "First start downloads the speech model (~0.5 GB) and pulls local models in the background."
echo "Log: $DATA/brain_service.log"
