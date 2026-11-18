#!/bin/bash
# One-time: creates build/ghidra/Moon from the game ELF and runs auto-analysis (slow).
source "$(dirname "$0")/env.sh"
mkdir -p "$MOON/build/ghidra"
cmd //c "$(cygpath -w "$GHIDRA/support/analyzeHeadless.bat")" \
    "$(cygpath -w "$MOON/build/ghidra")" Moon -import "$(cygpath -w "$ELF")" \
    -processor r5900:LE:32:default -cspec default -overwrite -max-cpu "$(nproc)" \
    < /dev/null > "$MOON/logs/ghidra_import.log" 2>&1
tail -n 3 "$MOON/logs/ghidra_import.log"
