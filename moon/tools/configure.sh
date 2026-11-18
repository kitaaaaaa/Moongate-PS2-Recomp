#!/bin/bash
# Builds the recompiler and analyzer (build/tools), then configures the runtime (build/runtime).
source "$(dirname "$0")/env.sh"
ARCH_FLAGS="${PS2X_ARCH_FLAGS:--march=native}"
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        -DCMAKE_C_FLAGS="$ARCH_FLAGS" -DCMAKE_CXX_FLAGS="$ARCH_FLAGS" -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF)
cmake -S "$RECOMP_SRC" -B "$MOON/build/tools" "${COMMON[@]}" \
    -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_ANALYZER=ON -DPS2X_BUILD_RUNTIME=OFF "$@"
cmake --build "$MOON/build/tools"
mkdir -p "$MOON/build/recomp"
cmake -S "$RECOMP_SRC" -B "$MOON/build/runtime" "${COMMON[@]}" \
    -DPS2X_BUILD_RUNTIME=ON -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF \
    -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_SHOW_WINDOWS_CONSOLE=ON -DOPENGL_VERSION=4.3 \
    -DPS2X_GAME_SOURCE_DIR="$MOON/src" -DPS2X_GENERATED_DIR="$MOON/build/recomp" "$@"
