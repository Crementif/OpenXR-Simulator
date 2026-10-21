#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tool_root="${SIMXR_TOOL_ROOT:-$HOME/.local/share/bettervr-tools/simulator}"
cmake_bin=$(command -v cmake || true)
ninja_bin=$(command -v ninja || true)
[ -n "$cmake_bin" ] || cmake_bin="$tool_root/venv/bin/cmake"
[ -n "$ninja_bin" ] || ninja_bin="$tool_root/venv/bin/ninja"
if [ ! -x "$cmake_bin" ] || [ ! -x "$ninja_bin" ]; then
    echo 'Install cmake/ninja in PATH, or run: python3 -m venv ~/.local/share/bettervr-tools/simulator/venv && ~/.local/share/bettervr-tools/simulator/venv/bin/pip install cmake ninja' >&2
    exit 1
fi
args=(-S "$root" -B "${SIMXR_BUILD_DIR:-$root/build-linux}" -G Ninja
    "-DCMAKE_MAKE_PROGRAM=$ninja_bin" -DCMAKE_BUILD_TYPE=RelWithDebInfo)
if [ -f "$tool_root/Vulkan-Headers/include/vulkan/vulkan.h" ]; then
    args+=("-DSIMXR_VULKAN_INCLUDE_DIR=$tool_root/Vulkan-Headers/include")
fi
if [ -f "$tool_root/SDL3/include/SDL3/SDL.h" ]; then
    args+=("-DSIMXR_SDL3_INCLUDE_DIR=$tool_root/SDL3/include")
fi
"$cmake_bin" "${args[@]}" "$@"
"$cmake_bin" --build "${SIMXR_BUILD_DIR:-$root/build-linux}" --parallel "${SIMXR_BUILD_JOBS:-4}"
ctest_bin="${cmake_bin%/*}/ctest"
"$ctest_bin" --test-dir "${SIMXR_BUILD_DIR:-$root/build-linux}" --output-on-failure
