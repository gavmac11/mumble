#!/usr/bin/env bash
# Disposable Debian container build; never install these packages on the host.
set -euo pipefail

if [[ ! -f /.dockerenv || "${PILOT_BUILD_CONTAINER:-}" != 1 ]]; then
  printf '%s\n' 'Run inside the explicitly designated pilot build container.' >&2
  exit 2
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build python3 pkg-config ca-certificates git \
  libssl-dev libprotobuf-dev protobuf-compiler libboost-dev qt6-base-dev \
  libsqlite3-dev libcap-dev

# GCC 12 emits a known restrict false positive for the existing std::string
# concatenation in DataType.cpp. Keep that warning visible in this pilot build.
# https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105651
cmake -S /source -B /build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS=-Wno-error=restrict \
  -Dclient=OFF -Dserver=ON -Dtests=OFF -Dplugins=OFF \
  -Dice=OFF -Dzeroconf=OFF -Denable-mysql=OFF -Denable-postgresql=OFF \
  -Dlto=OFF
cmake --build /build --target mumble-server --parallel 2
find /build -type f -name mumble-server -executable -print
