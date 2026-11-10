#!/usr/bin/env bash
# Build the AirBeam HAL plugin, install it, restart coreaudiod.
# usage: noah/install.sh [latency_frames]   (default 0: measured ~200 ms total vs ~520 ms at 11025)
set -e
cd "$(dirname "$0")/.."
LAT="${1:-0}"
cmake -S . -B build -GNinja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-DAIRBEAM_DEFAULT_LATENCY=$LAT" >/dev/null
cmake --build build --target AirBeamASP | tail -1
D=/Library/Audio/Plug-Ins/HAL/AirBeamASP.driver
sudo rm -rf "$D" && sudo cp -r build/source/AirBeamASP/AirBeamASP.driver "$D"
sudo codesign --force --deep -s - "$D" 2>/dev/null
sudo killall coreaudiod
echo "installed with latency $LAT frames"
