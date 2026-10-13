#!/bin/bash
set -e
source "$(dirname "$0")/env.sh"
cd /tmp
curl -fsSL -o gcl.zip https://github.com/Cuyler36/Ghidra-GameCube-Loader/releases/download/1.3.1/GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip
rm -rf gcl && mkdir gcl && unzip -q gcl.zip -d gcl
ls gcl
cp -r gcl/* "$GHIDRA_INSTALL_DIR/Ghidra/Extensions/"
ls "$GHIDRA_INSTALL_DIR/Ghidra/Extensions/"
find "$GHIDRA_INSTALL_DIR/Ghidra/Extensions/GameCubeLoader" -maxdepth 3 | head -40
grep -rho 'id="[^"]*"' "$GHIDRA_INSTALL_DIR/Ghidra/Extensions/GameCubeLoader" --include=*.ldefs || true
