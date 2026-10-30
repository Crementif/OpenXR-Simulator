#!/usr/bin/env python3
"""Run the native soak with a private probe and clean up only that probe's processes."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def matching_processes(directory):
    marker = ("SIMXR_DATA_DIR=" + str(directory)).encode()
    for process in Path("/proc").glob("[0-9]*"):
        try:
            if marker in (process / "environ").read_bytes().split(b"\0"):
                yield int(process.name)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("out/build/merge-qa"))
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory")
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()
    if args.seed < 0 or args.seconds <= 0:
        parser.error("Use a nonnegative seed and a positive duration")
    layers = os.environ.get("VK_LAYER_PATH", "").split(os.pathsep)
    manifests = [Path(path) / "VkLayer_khronos_validation.json" for path in layers if path]
    versions = [json.loads(path.read_text())["layer"]["api_version"] for path in manifests if path.is_file()]
    if not versions or any(tuple(map(int, version.split("."))) < (1, 4, 363) for version in versions):
        parser.error("Set VK_LAYER_PATH and LD_LIBRARY_PATH to SDK 1.4.363 or newer")
    output, build = args.output.resolve(), args.build.resolve()
    data = output / "session"
    if len(os.fsencode(data / "control.2147483647.sock")) >= 108:
        parser.error("Output path is too long for a Unix socket; use a short directory under /tmp")
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("SIMXR_"):
            del env[key]
    env.update(SIMXR_DATA_DIR=str(data),
               SIMXR_LAUNCH_QA_DIRECTORY=str(data),
               SIMXR_LAUNCH_QA_EXECUTABLE=str(build / "bin/xr_probe_linux"),
               XR_RUNTIME_JSON=str(build / "bin/openxr_simulator.json"),
               SIMXR_SOAK_QA="1", SIMXR_SOAK_SEED=str(args.seed), SIMXR_SOAK_SECONDS=str(args.seconds),
               SIMXR_VULKAN_VALIDATION="1", VK_LAYER_ENABLES="VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
    log = output / "soak.log"
    print(f"seed={args.seed} seconds={args.seconds} log={log}", flush=True)
    returncode = 1
    try:
        with log.open("w") as stream:
            result = subprocess.run([str(build / "companion_interaction_tests")], env=env,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=args.seconds + 180)
            returncode = result.returncode
    except subprocess.TimeoutExpired:
        print("Soak exceeded its deadline", flush=True)
    finally:
        # The companion intentionally leaves launched apps alive. Never kill by executable name.
        for pid in matching_processes(data):
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 3
        while list(matching_processes(data)) and time.monotonic() < deadline:
            time.sleep(.05)
        for pid in matching_processes(data):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    text = log.read_text(errors="replace")
    validation_error = "validation error" in text.lower() or "VUID-" in text
    passed = returncode == 0 and not validation_error and f"PASS soak seed={args.seed}" in text
    print("\n".join(text.splitlines()[-8:]))
    if not passed:
        trace = data / "soak-last-actions.txt"
        if trace.exists():
            print(trace.read_text())
        print(f"FAIL: exit={returncode}, validation_error={validation_error}; full log: {log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
