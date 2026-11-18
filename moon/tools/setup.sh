#!/bin/bash
# Builds the game from the user's own disc image and writes Moongate.exe at the top of the checkout (setup.bat runs this).
# Usage: setup.sh <Sega Ages 2500 Vol. 31 Dennou Senki Virtual On (SLPM-62767) .bin, .cue or .iso>
# Steps already done are skipped on a second run; recompiling and building always run.
set -euo pipefail
TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DISC="$(cygpath -u "$1")"
[ -f "$DISC" ] || { echo "no disc image at $1" >&2; exit 1; }
# A .cue names its data track: read that .bin (next to the .cue).
if [[ "${DISC,,}" == *.cue ]]; then
    BIN="$(sed -n 's/^[[:space:]]*FILE[[:space:]]*"\(.*\)".*/\1/p' "$DISC" | head -n 1 | tr -d '\r')"
    DISC="$(dirname "$DISC")/$BIN"
    [ -f "$DISC" ] || { echo "the .cue names $BIN, which is not next to it" >&2; exit 1; }
fi
TC="$TOP/toolchain"
mkdir -p "$TC/downloads"

fetch() {  # url sha256 file folder-it-unpacks-to [folder to unpack into, default toolchain]
    [ -d "$TC/$4" ] && return
    echo "Downloading $(basename "$1")..."
    curl -L --fail -o "$TC/downloads/$3" "$1"
    echo "$2 *$TC/downloads/$3" | sha256sum -c --quiet
    unzip -q -o "$TC/downloads/$3" -d "$TC/${5:-}"
}
fetch https://github.com/adoptium/temurin25-binaries/releases/download/jdk-25.0.5%2B8/OpenJDK25U-jdk_x64_windows_hotspot_25.0.5_8.zip \
    636bdc13dc649ddc7d5921c5c7d23cca6e44fd19d662b45cc9c02135a6c96c42 jdk25.zip jdk-25.0.5+8
fetch https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip \
    ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db ghidra.zip ghidra_12.1.4_PUBLIC
# Ghidra's PS2 (r5900) processor.
fetch https://github.com/chaoticgd/ghidra-emotionengine-reloaded/releases/download/v2.1.39/ghidra_12.1.4_PUBLIC_20261029_ghidra-emotionengine-reloaded.zip \
    088369c4d7de43cc95dbf254936dfed45eae5eddfcaf8b928189344bcc6a4dac ee-ext.zip \
    ghidra_12.1.4_PUBLIC/Ghidra/Extensions/ghidra-emotionengine-reloaded ghidra_12.1.4_PUBLIC/Ghidra/Extensions

unset MOON_TOOLCHAIN
source "$TOP/moon/tools/env.sh"
cd "$MOON"

echo "Extracting the disc..."
python3 tools/extract_iso.py "$DISC" iso_jp > logs/setup_extract.log
[ -f iso_jp/SLPM_627.67 ] || { echo "SLPM_627.67 is not on this disc: Sega Ages 2500 Vol. 31 Dennou Senki Virtual On (SLPM-62767) is needed" >&2; exit 1; }
# Memory card folder (the runtime's default: mc0 next to the ELF).
mkdir -p iso_jp/mc0 src  # src: game sources the runtime build compiles in (none yet)

if [ ! -f build/tools/ps2xRecomp/ps2_recomp.exe ] || [ ! -f build/runtime/build.ninja ]; then
    echo "Configuring the build (downloads the libraries it uses)..."
    bash tools/configure.sh > logs/setup_configure.log 2>&1 || { tail -n 30 logs/setup_configure.log; exit 1; }
fi
if [ ! -d build/ghidra/Moon.rep ]; then
    echo "Analysing the game program with Ghidra (a few minutes)..."
    bash tools/ghidra_import.sh
    echo "Finding the functions only reached through pointers..."
    bash tools/ghidra_gaps.sh
fi
echo "Exporting the function list..."
bash tools/ghidra_export.sh
echo "Lifting the VU1 microprogram..."
python3 tools/vu1lift_moon.py iso_jp/SLPM_627.67 config/vu1_targets.json build/vu1census > logs/setup_vu1.log
echo "Recompiling and building (this takes a while)..."
bash tools/recomp.sh

echo "Writing Moongate.exe..."
mkdir -p bin
cp build/runtime/ps2xRuntime/ps2EntryRunner.exe bin/
# The DLLs it needs from the toolchain, so the game runs without it.
ldd bin/ps2EntryRunner.exe | awk '$3 ~ "^/clang64/" {print $3}' | sort -u | xargs -r cp -t bin/
clang++ -std=c++20 -O2 -municode -mwindows -static tools/moon_launcher.cpp -o "$TOP/Moongate.exe"
echo "Setup finished."
