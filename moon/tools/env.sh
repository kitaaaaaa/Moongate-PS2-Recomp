# Sourced by the other scripts in this folder. Run them from the MSYS2 CLANG64 shell:
#   toolchain\msys64\usr\bin\env.exe MSYSTEM=CLANG64 CHERE_INVOKING=1 /usr/bin/bash -lc "moon/tools/<script>.sh"
# The toolchain is the one setup.bat installs into toolchain\; MOON_TOOLCHAIN points at another one (e.g. Marz's).
set -euo pipefail
MOON_TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export MOON="$MOON_TOP/moon"
export BASE="$MOON_TOP/external/gitaroo-man-native"
export RECOMP_SRC="$BASE/external/PS2Recomp"
export TOOLCHAIN="${MOON_TOOLCHAIN:-$MOON_TOP/toolchain}"
export GHIDRA="$TOOLCHAIN/ghidra_12.1.4_PUBLIC"
JDK="$(ls -d "$TOOLCHAIN"/jdk-25* | head -1)"
export JAVA_HOME="$(cygpath -w "$JDK")"
# Ghidra settings/cache on J: (C: is nearly full).
GHIDRA_HOME="$MOON/build/ghidra_home"
mkdir -p "$GHIDRA_HOME/local" "$GHIDRA_HOME/roaming" "$GHIDRA_HOME/tmp"
export LOCALAPPDATA="$(cygpath -w "$GHIDRA_HOME/local")"
export APPDATA="$(cygpath -w "$GHIDRA_HOME/roaming")"
export TEMP="$(cygpath -w "$GHIDRA_HOME/tmp")"
export TMP="$TEMP"
export ELF="$MOON/iso_jp/SLPM_627.67"
mkdir -p "$MOON/build" "$MOON/logs"
