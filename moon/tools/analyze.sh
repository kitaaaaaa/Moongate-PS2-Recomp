#!/bin/bash
# Matches the stripped ELF against PS2Recomp's SCE library signature database and writes
# config/symbols/sdk.txt (name address lines that ghidra_export.sh applies).
source "$(dirname "$0")/env.sh"
cd "$MOON"
build/tools/ps2xAnalyzer/ps2_analyzer "$(cygpath -m "$ELF")" build/analyzer.toml > logs/analyzer.log 2>&1 || { tail -n 20 logs/analyzer.log; exit 1; }
python3 tools/analyzer_symbols.py build/analyzer.toml config/symbols/sdk.txt
