#!/bin/sh
# Fetch everything MBot builds against or loads, at pinned versions, into third_party/ and models/.
# Nothing here is redistributed by MBot; see THIRD_PARTY_NOTICES.md for each licence.
#   ./scripts/fetch_deps.sh            all of it
#   NO_VOICE_ID=1 ./scripts/fetch_deps.sh   skip sherpa-onnx + speaker model (voice recognition off)
#   WHISPER_BASE=1 ./scripts/fetch_deps.sh  also fetch the smaller ggml-base.en model
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$ROOT/third_party"
MODELS="$ROOT/models"
mkdir -p "$TP" "$MODELS"

pinned() {  # pinned <dir> <git url> <commit>
  if [ -d "$TP/$1/.git" ]; then echo "$1: present"; return; fi
  echo "$1: cloning $2 @ $3"
  git init -q "$TP/$1"
  git -C "$TP/$1" remote add origin "$2"
  git -C "$TP/$1" fetch -q --depth 1 origin "$3"
  git -C "$TP/$1" checkout -q "$3"
}
fetch() {  # fetch <url> <dest>
  if [ -s "$2" ]; then echo "$(basename "$2"): present"; return; fi
  echo "$(basename "$2"): downloading"
  curl -fL --retry 3 -o "$2.part" "$1" && mv "$2.part" "$2"
}

# Anki Vector SDK protocol definitions (Apache-2.0)
pinned vector-sdk https://github.com/kercre123/wirepod-vector-python-sdk.git 065fc197d592d76a164d64dcf0a768183ab37855
# whisper.cpp (MIT)
pinned whisper.cpp https://github.com/ggml-org/whisper.cpp.git 5eaee0cc9024eb9e9a99469199920c8b7f5cb9ec
# vector-cloud (MIT) - robot/build.sh builds the mic streamer inside it
pinned vector-cloud https://github.com/digital-dream-labs/vector-cloud.git bba1d08028b18c504d6cad781790f4c4f2e3cf7d
mkdir -p "$TP/vector-cloud/cmd/mictap"

# Whisper speech-to-text weights (MIT, OpenAI; converted by the ggml project)
fetch https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin "$MODELS/ggml-small.en.bin"
[ -n "$WHISPER_BASE" ] && fetch https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin "$MODELS/ggml-base.en.bin"

if [ -z "$NO_VOICE_ID" ]; then
  # sherpa-onnx C API + ONNX Runtime, prebuilt (Apache-2.0 / MIT)
  if [ ! -f "$TP/sherpa-onnx/lib/libsherpa-onnx-c-api.so" ]; then
    V=v2.0.3
    T=sherpa-onnx-$V-linux-x64-shared-no-tts-lib
    tmp=$(mktemp -d)
    fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/$V/$T.tar.bz2" "$tmp/s.tar.bz2"
    tar xjf "$tmp/s.tar.bz2" -C "$tmp"
    mkdir -p "$TP/sherpa-onnx/include/sherpa-onnx/c-api"
    cp -r "$tmp/$T/lib" "$TP/sherpa-onnx/"
    # the prebuilt package has no headers: the C API header from the same release tag
    curl -fsSL -o "$TP/sherpa-onnx/include/sherpa-onnx/c-api/c-api.h" \
         "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/$V/sherpa-onnx/c-api/c-api.h"
    curl -fsSL -o "$TP/sherpa-onnx/LICENSE" "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/$V/LICENSE"
    rm -rf "$tmp"
  else
    echo "sherpa-onnx: present"
  fi
  # WeSpeaker CAM++ speaker model (Apache-2.0; trained on VoxCeleb, CC BY 4.0)
  fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_CAM++_LM.onnx" \
        "$MODELS/speaker-campplus-en.onnx"
fi
echo "done. Next: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
