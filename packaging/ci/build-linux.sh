#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
work=${1:?Give an absolute new CI work directory}
case "$work" in /*) ;; *) printf '%s\n' 'CI work directory must be absolute' >&2; exit 1 ;; esac
if [ -e "$work" ]; then
    printf '%s\n' 'Refusing to reuse a CI work directory' >&2
    exit 1
fi
mkdir -p "$work"
python3 "$root/tests/packaging/test_linux_abi.py"
python3 "$root/packaging/ci/fetch-deps.py" --platform linux --output "$work/deps"
cmake --preset release -S "$root" -B "$work/build" \
    -DSIMXR_BUILD_PROBES=ON -DSIMXR_BUILD_HEADSET=ON \
    "-DSIMXR_VULKAN_INCLUDE_DIR=$work/deps/Vulkan-Headers/include" \
    "-DSIMXR_RELEASE_REPOSITORY=${SIMXR_RELEASE_REPOSITORY:-}"
cmake --build "$work/build" --parallel "${SIMXR_CI_JOBS:-4}" --target \
    openxr_simulator simxr_companion simxr_runner simxr_headset_host xr_probe_linux game_bridge_probe \
    companion_settings_tests update_checker_tests mcp_simulation_command_tests mcp_history_tests startup_failure_tests \
    stereo_surface_tests stereo_consistency_tests mcp_image_encode_tests mcp_application_tests package_documentation_tests \
    simxr_native_regression_tests
ctest --test-dir "$work/build" --output-on-failure -R '^(companion_settings|update_checker|mcp_simulation_commands|mcp_history|mcp_connector_cancellation|mcp_connector_configuration|mcp_connector_protocol|native_mcp_game_bridge|startup_failure_format|stereo_surface|stereo_consistency|mcp_image_encode|mcp_application|package_documentation|package_documentation_open)$'
ctest --test-dir "$work/build" --output-on-failure --no-tests=error -L '^native-regression$'
python3 "$root/packaging/linux/build-package.py" --build "$work/build" --output "$work/packages"
python3 - "$work/build/generated/simxr/Version.h" "$work/packages/VERSION" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
match = re.search(r'Version\[\]\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"', text)
if not match:
    raise SystemExit("Configured version not found")
Path(sys.argv[2]).write_text(match.group(1) + "\n", encoding="ascii")
PY
