#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [ "$#" -eq 0 ]; then
    echo "Usage: $0 application [arguments...]" >&2
    echo "For the loader probe: SIMXR_DATA_DIR=$root/build-linux/probe-manual $0 $root/bin/xr_probe_linux" >&2
    exit 2
fi
[ -f "$root/bin/openxr_simulator.json" ] || { echo 'Run scripts/build-linux.sh first.' >&2; exit 1; }
export XR_RUNTIME_JSON="$root/bin/openxr_simulator.json"
export SIMXR_DATA_DIR="${SIMXR_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/OpenXR-Simulator}"
mkdir -p -- "$SIMXR_DATA_DIR"
exec "$@"
