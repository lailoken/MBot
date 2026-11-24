#!/bin/sh
# Build the on-robot pieces (armv7 / glibc) in a throwaway golang container:
#   out/libmbotmic.so  - LD_PRELOAD mic shim for vic-anim
#   out/shimtest       - on-robot self-test for the shim
#   out/mictap         - mic streamer (raw constant audio + gated fallback)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MBOT=$(dirname "$HERE")
mkdir -p "$HERE/out"
cp "$HERE/mictap/main.go" "$MBOT/third_party/vector-cloud/cmd/mictap/main.go"
podman run --rm -v "$HERE:/robot:Z" -v "$MBOT/third_party/vector-cloud:/vc:Z" \
  -v mbot-gocache:/root/go docker.io/library/golang:1.22 sh -ec '
    if ! command -v arm-linux-gnueabihf-gcc >/dev/null; then
      apt-get update -qq && apt-get install -y -qq gcc-arm-linux-gnueabihf >/dev/null
    fi
    arm-linux-gnueabihf-gcc -O2 -Wall -fPIC -shared -o /robot/out/libmbotmic.so /robot/shim/mbotmic.c -ldl
    arm-linux-gnueabihf-gcc -O2 -Wall -o /robot/out/shimtest /robot/shim/shimtest.c
    cd /vc && GOARCH=arm GOARM=7 CGO_ENABLED=0 go build -tags vicos -trimpath -ldflags="-s -w" -o /robot/out/mictap ./cmd/mictap
  '
ls -la "$HERE/out"
