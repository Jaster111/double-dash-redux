#!/bin/bash
# Start the persistent REA MCP daemon for main.elf (Gekko language).
source "$(dirname "$0")/env.sh"
export REA_GHIDRA_LANGUAGE_ID=PowerPC:BE:32:Gekko_Broadway
[ -n "$SEEDS" ] && export REA_GHIDRA_SEED_FILE="$SEEDS"
cd "$WORK"
exec python3 "$PROJ/tools/wsl/rea_daemon.py" 8765
