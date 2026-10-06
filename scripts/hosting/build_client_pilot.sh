#!/usr/bin/env bash
# Disposable Debian build for the client change; never install these on the host.
set -euo pipefail
if [[ ! -f /.dockerenv || "${PILOT_BUILD_CONTAINER:-}" != 1 ]]; then
  printf '%s\n' 'Run inside the explicitly designated pilot build container.' >&2
  exit 2
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  libqt6svg6-dev qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools \
  libpoco-dev libsndfile1-dev libopus-dev libasound2-dev libxi-dev \
  libavcodec-dev libswscale-dev libavutil-dev libavdevice-dev libavformat-dev \
  libpipewire-0.3-dev clang-format
cmake -S /source -B /client-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Wno-error=restrict \
  -Dclient=ON -Dserver=OFF -Dtests=ON -Dplugins=OFF \
  -Dice=OFF -Dzeroconf=OFF -Denable-mysql=OFF -Denable-postgresql=OFF \
  -Dlto=OFF -Doverlay=OFF -Doverlay-xcompile=OFF -Dmanual-plugin=OFF \
  -Djackaudio=OFF -Dportaudio=OFF -Dpulseaudio=OFF -Dspeechd=OFF \
  -Drnnoise=OFF -Dscreen-sharing=ON -Dtranslations=OFF -Dbundle-qt-translations=OFF
cmake --build /client-build --target mumble TestVideoPacketPacer TestVideoFramePacketizer TestVideoQualityProfile --parallel 2
ctest --test-dir /client-build --output-on-failure -R '^TestVideo(PacketPacer|FramePacketizer|QualityProfile)$'
