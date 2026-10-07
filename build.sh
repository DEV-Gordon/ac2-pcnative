#!/bin/bash
# Builds the native port from YOUR OWN disc image:
#   ./build.sh "/path/to/Ace Combat 2 (USA).bin"
# 1) extracts the game executables, 2) recompiles them to C, 3) compiles everything.
# Needs: python3, cmake, ninja, a C/C++ compiler, SDL2 development files.
set -euo pipefail
cd "$(dirname "$0")"
DISC="${1:?usage: ./build.sh /path/to/disc.bin}"
python3 tools/extract.py "$DISC" work
python3 tools/recomp.py recomp.json gen
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo
echo "Done: ./build/ac2native   (select the same disc in the launcher)"
