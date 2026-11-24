# Third-party notices

MBot is built on other people's work. This file lists every third-party component MBot uses, its
licence, where it comes from, and how MBot uses it. **None of these are redistributed in this
repository**: sources are fetched by `scripts/fetch_deps.sh` (pinned commits), system libraries
come from your distribution, and models, firmware and services are installed or used by you under
their own terms. If you distribute **binaries** built from MBot, you must ship the licence texts of
the components compiled into them (sections 1 and 2).

---

## 1. Compiled into `mbot` (the PC program)

| Component | Licence | Source | Used for |
|---|---|---|---|
| **Anki Vector SDK protocol definitions** (`anki_vector/messaging/*.proto`), Copyright (c) 2018 Anki, Inc. | **Apache-2.0** | via [kercre123/wirepod-vector-python-sdk](https://github.com/kercre123/wirepod-vector-python-sdk) @ `065fc19` | generating the C++ gRPC client for the robot's SDK gateway. **Modification:** at build time `CMakeLists.txt` removes the `import "google/api/annotations.proto";` line (REST hints not needed); the modified copies exist only under `build/` |
| **whisper.cpp / ggml**, Copyright (c) 2023-2026 The ggml authors | **MIT** | [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp) @ `5eaee0c` | local speech-to-text (statically linked) |
| **nlohmann/json**, Copyright (c) 2013-2025 Niels Lohmann | **MIT** | distribution package (`json-devel`) | JSON (header-only, compiled in) |
| **gRPC C++**, Copyright The gRPC Authors | **Apache-2.0** | distribution package (`grpc-devel`) | robot SDK transport (dynamically linked) |
| **Protocol Buffers**, Copyright Google LLC | **BSD-3-Clause** | distribution package (`protobuf-devel`) | message encoding (dynamically linked) |
| **OpenSSL 3** | **Apache-2.0** | distribution package (`openssl-devel`) | TLS to the robot and to the Claude API (dynamically linked) |
| **sherpa-onnx** (C API), Copyright (c) k2-fsa / Xiaomi Corp. | **Apache-2.0** | prebuilt `v2.0.3` from [k2-fsa/sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) releases (fetched by `fetch_deps.sh`, optional) | voice recognition (speaker embeddings; dynamically linked) |
| **ONNX Runtime**, Copyright (c) Microsoft Corporation | **MIT** | shipped inside the sherpa-onnx package | runs the speaker model (dynamically linked) |

## 2. Compiled into the robot-side programs (`robot/`)

| Component | Licence | Source | Used for |
|---|---|---|---|
| **vector-cloud**, Copyright (c) 2021 Digital Dream Labs | **MIT** | [digital-dream-labs/vector-cloud](https://github.com/digital-dream-labs/vector-cloud) @ `bba1d08` | `robot/mictap` is built inside it and uses its `internal/ipc` and `internal/clad/cloud` packages |
| google.golang.org/grpc v1.40.0 | Apache-2.0 | Go module (via vector-cloud) | linked into the `mictap` binary |
| google.golang.org/genproto (2021-08-30) | Apache-2.0 | Go module | linked into `mictap` |
| github.com/golang/protobuf v1.5.2, google.golang.org/protobuf v1.27.1 | BSD-3-Clause | Go modules | linked into `mictap` |
| golang.org/x/net, x/sys, x/text | BSD-3-Clause | Go modules | linked into `mictap` |
| The Go toolchain/runtime | BSD-3-Clause | `golang:1.22` container image | building `mictap` |
| GNU C Library (on the robot) | LGPL-2.1+ | robot firmware | `libmbotmic.so` links dynamically against the robot's own glibc |
| Debian `gcc-arm-linux-gnueabihf` | GPL-3.0+ with GCC Runtime Library Exception | container package | cross-compiling the shim (the exception covers the compiled output) |

`robot/shim/mbotmic.c`, `robot/shim/shimtest.c`, `tools/mindview.py` (Python standard library only) and
`web/*.html` (no external libraries) and `scripts/*` are original MBot code.

---

## 3. Used at runtime, installed by you (not distributed)

### AI models and services
| Component | Licence / terms | Notes |
|---|---|---|
| **Whisper** models (`ggml-small.en.bin`, `ggml-base.en.bin`), weights by OpenAI, converted by the ggml project | **MIT** | downloaded by `fetch_deps.sh` from huggingface.co/ggerganov/whisper.cpp |
| **WeSpeaker CAM++** speaker model (`speaker-campplus-en.onnx`, trained on VoxCeleb), from [wenet-e2e/wespeaker](https://github.com/wenet-e2e/wespeaker), ONNX export by sherpa-onnx | **Apache-2.0** (WeSpeaker); VoxCeleb training data is CC BY 4.0 | downloaded by `fetch_deps.sh`; voiceprints are computed and stored locally only |
| **Ollama** | MIT | local model server |
| **MiniCPM-V** (`minicpm-v`, local vision) | **MiniCPM Model License v1.0**: free for research; **commercial use requires registration** with the model authors | default `--vision-model`; replace it if your use isn't covered |
| **Qwen2.5 7B** (`qwen2.5:7b`, default local model: dreaming/reflection, background thoughts, fallback) | **Apache-2.0** | override with `--fast-model` / `MBOT_LOCAL_MODEL` |
| Qwen2.5 3B (`qwen2.5:3b`) | Qwen RESEARCH LICENSE AGREEMENT: **non-commercial** | lighter alternative; fine for personal use only |
| Qwen2.5 14B (`qwen2.5:14b`) | Apache-2.0 | optional local fallback for dreaming / memory consolidation |
| Qwen2.5-VL 7B (`qwen2.5vl:7b`) | Apache-2.0 | optional; crashes on text-only prompts in some Ollama builds |
| **Claude** (Anthropic API) | Anthropic's API terms | you provide your own API key; usage is billed to you |

### Robot firmware and community tools (only referenced in the docs)
| Component | Notes |
|---|---|
| **WireOS** (community Vector firmware) | flashed via the community web tool; see its project for licence/terms |
| **websetup.skittle.dev** (community Vector web setup / flasher) | used to unlock/flash the robot; not part of MBot |
| **wire-pod** | MBot's token-forging helper (kept outside this repo) reimplements wire-pod's published token-hash scheme so the SDK can authenticate without a cloud account. Credit to the wire-pod authors |
| Anki / Digital Dream Labs Vector firmware | the robot's own software (vic-anim, vic-engine, ...). MBot does not modify these binaries; the mic shim is loaded alongside vic-anim with `LD_PRELOAD` |

---

## Trademarks

"Vector" and "Anki" are trademarks of their respective owners (now Digital Dream Labs). MBot is an
independent community project, not affiliated with or endorsed by Anki, Digital Dream Labs, the WireOS
or wire-pod projects, Anthropic, or the model authors.

---

## Licence texts

- MIT: https://opensource.org/license/mit, as included in each project's `LICENSE` file
  (`third_party/whisper.cpp/LICENSE`, `third_party/vector-cloud/LICENSE`).
- Apache-2.0: https://www.apache.org/licenses/LICENSE-2.0 (`third_party/vector-sdk/LICENSE.txt`).
- BSD-3-Clause: https://opensource.org/license/bsd-3-clause

When distributing binaries, copy those `LICENSE` files alongside them.
