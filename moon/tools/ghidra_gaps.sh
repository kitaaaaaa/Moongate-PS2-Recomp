#!/bin/bash
# One-time, after ghidra_import.sh: clears data the analysis put in .text (CleanTextData.java), then creates functions for code only reached through pointers (CreateGapFunctions.java).
source "$(dirname "$0")/env.sh"
SCRIPTS="$MOON/build/ghidra_scripts"
mkdir -p "$SCRIPTS"
cp "$MOON/tools/ghidra/CleanTextData.java" "$MOON/tools/ghidra/CreateGapFunctions.java" "$SCRIPTS/"
cmd //c "$(cygpath -w "$GHIDRA/support/analyzeHeadless.bat")" \
    "$(cygpath -w "$MOON/build/ghidra")" Moon -process SLPM_627.67 -noanalysis \
    -scriptPath "$(cygpath -w "$SCRIPTS")" -postScript CleanTextData.java -postScript CreateGapFunctions.java \
    < /dev/null > "$MOON/logs/ghidra_gaps.log" 2>&1
grep -E "CleanTextData:|CreateGapFunctions:|ERROR" "$MOON/logs/ghidra_gaps.log" | sort | uniq -c | sort -rn | head -20
