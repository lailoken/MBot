#!/bin/sh
# Build dependencies from your distribution. Fedora/Nobara (dnf) or Debian/Ubuntu (apt).
# The Vulkan bits let speech-to-text run on the GPU (any vendor); without them it builds for the CPU.
set -e
if command -v dnf >/dev/null; then
  sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config git curl bzip2 python3 podman \
      grpc-devel grpc-plugins protobuf-devel protobuf-compiler json-devel openssl-devel \
      vulkan-headers vulkan-loader-devel glslc spirv-headers-devel
elif command -v apt-get >/dev/null; then
  sudo apt-get update
  sudo apt-get install -y build-essential cmake pkg-config git curl bzip2 python3 podman \
      libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler nlohmann-json3-dev libssl-dev \
      libvulkan-dev glslc spirv-headers
else
  echo "Unknown distribution: install a C++17 compiler, cmake, gRPC + protobuf (with the C++ plugin),"
  echo "nlohmann-json, OpenSSL, curl, podman, and optionally the Vulkan headers/loader + glslc + SPIR-V headers."
  exit 1
fi
