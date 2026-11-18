#!/bin/bash
# Applies moon/config/symbols/*.txt to the Ghidra project and exports build/functions.csv plus the
# exporter's TOML (build/ghidra_export.toml), which gen_toml.py turns into the recompiler config.
source "$(dirname "$0")/env.sh"
cat "$MOON"/config/symbols/*.txt 2>/dev/null | grep -v '^#' | grep . > "$MOON/build/symbols_all.txt" || true
# One script folder: cmd splits arguments on ';', so a path list can't reach analyzeHeadless.bat.
SCRIPTS="$MOON/build/ghidra_scripts"
mkdir -p "$SCRIPTS"
cp "$BASE/tools/ghidra/ApplySymbols.java" "$RECOMP_SRC/ps2xRecomp/tools/ghidra/ExportPS2Functions.java" "$SCRIPTS/"
cmd //c "$(cygpath -w "$GHIDRA/support/analyzeHeadless.bat")" \
    "$(cygpath -w "$MOON/build/ghidra")" Moon -process SLPM_627.67 -noanalysis \
    -scriptPath "$(cygpath -w "$SCRIPTS")" \
    -postScript ApplySymbols.java "$(cygpath -w "$MOON/build/symbols_all.txt")" \
    -postScript ExportPS2Functions.java "$(cygpath -w "$MOON/build/ghidra_export.toml")" "$(cygpath -w "$MOON/build/functions.csv")" \
    < /dev/null > "$MOON/logs/ghidra_export.log" 2>&1
grep -E 'ApplySymbols:|Exported [0-9]+ functions' "$MOON/logs/ghidra_export.log"
