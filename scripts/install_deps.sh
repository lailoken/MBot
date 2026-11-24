#!/bin/sh
# Build dependencies from your distribution (Fedora / Nobara). Debian/Ubuntu equivalents:
#   sudo apt install build-essential cmake pkg-config git curl libgrpc++-dev protobuf-compiler-grpc \
#        libprotobuf-dev protobuf-compiler nlohmann-json3-dev libssl-dev podman python3
set -e
sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config git curl grpc-devel grpc-plugins protobuf-devel \
    protobuf-compiler json-devel openssl-devel podman python3 bzip2
