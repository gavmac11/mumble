#!/usr/bin/env bash
set -euo pipefail
if [[ ! -f /.dockerenv || "${PILOT_BUILD_CONTAINER:-}" != 1 ]]; then
  printf '%s\n' 'Run inside the explicitly designated pilot build container.' >&2
  exit 2
fi
g++ -std=c++20 -O2 -Wall -Wextra -Werror -fPIC -shared \
  -I/source/src/mumble $(pkg-config --cflags Qt6Core) \
  /source/scripts/hosting/pacer_bridge.cpp /source/src/mumble/VideoPacketPacer.cpp \
  -o /build/libmumble_pilot_pacer.so $(pkg-config --libs Qt6Core)
