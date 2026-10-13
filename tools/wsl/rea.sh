#!/bin/bash
# Run an arbitrary rea command inside the WSL analysis environment.
# Usage: rea.sh <rea args...>    (stdout = rea output)
source "$(dirname "$0")/env.sh"
[ -n "$LANG_ID" ] && export REA_GHIDRA_LANGUAGE_ID="$LANG_ID"
[ -n "$SEEDS" ] && export REA_GHIDRA_SEED_FILE="$SEEDS"
exec rea "$@"
