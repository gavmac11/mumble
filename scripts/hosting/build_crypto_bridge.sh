#!/usr/bin/env bash
set -euo pipefail
if [[ ! -f /.dockerenv || "${PILOT_BUILD_CONTAINER:-}" != 1 ]]; then
  printf '%s\n' 'Run inside the explicitly designated pilot build container.' >&2
  exit 2
fi
g++ -std=c++20 -O2 -fPIC -shared \
  -I/source/src -I/source/3rdparty/arc4random \
  /source/scripts/hosting/ocb_bridge.cpp \
  /source/src/crypto/CryptStateOCB2.cpp /source/src/crypto/CryptState.cpp \
  /source/src/crypto/CryptographicRandom.cpp /source/src/Timer.cpp \
  /source/3rdparty/arc4random/arc4random_uniform.cpp \
  -o /build/libmumble_pilot_ocb.so \
  $(pkg-config --cflags --libs Qt6Core openssl)
