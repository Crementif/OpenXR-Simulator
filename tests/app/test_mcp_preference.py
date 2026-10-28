#!/usr/bin/env python3
"""Verify saved and connector-started MCP enablement in the real app."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from test_mcp_connector import Client, eventually


def main():
    app, connector, helper = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="simxr-mcp-preference-") as temporary:
        root = Path(temporary)
        settings = root / "settings"
        env = dict(os.environ, SIMXR_COMPANION_SETTINGS=str(settings), SIMXR_DATA_DIR=temporary)
        env.pop("SIMXR_MCP_TOKEN", None)
        env.pop("XR_RUNTIME_JSON", None)
        groups = []
        processes = []
        with (root / "app.log").open("wb") as log:
            try:
                subprocess.run([helper, "--write-settings", str(settings), "1"], check=True, env=env)
                enabled_env = dict(env, SIMXR_MCP_DISCOVERY_DIR=str(root / "enabled"))
                manual = subprocess.Popen([app, "--hidden", "--validation"], env=enabled_env, stdout=log,
                                          stderr=subprocess.STDOUT, start_new_session=True)
                processes.append(manual)
                groups.append(manual.pid)

                def record(directory):
                    files = list(directory.glob("*.json"))
                    return json.loads(files[0].read_text()) if len(files) == 1 else None

                current = eventually(lambda: record(root / "enabled"))
                assert current["process_id"] == manual.pid
                bridge = subprocess.Popen([connector, "--app", app, "--windowless"], env=enabled_env, stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
                                          start_new_session=True)
                processes.append(bridge)
                groups.append(bridge.pid)
                client = Client(bridge)
                params = {"protocolVersion": "2025-11-25", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "1"}}
                assert "result" in client.rpc("initialize", params)
                assert record(root / "enabled")["process_id"] == manual.pid
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0 and manual.poll() is None
                os.killpg(manual.pid, signal.SIGTERM)
                manual.wait(timeout=10)

                subprocess.run([helper, "--write-settings", str(settings), "0"], check=True, env=env)
                bootstrap_env = dict(env, SIMXR_MCP_DISCOVERY_DIR=str(root / "bootstrap"))
                bootstrap = subprocess.Popen([connector, "--app", app], env=bootstrap_env, stdin=subprocess.PIPE,
                                             stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
                                             start_new_session=True)
                processes.append(bootstrap)
                groups.append(bootstrap.pid)
                client = Client(bootstrap)
                assert "result" in client.rpc("initialize", params)
                boot_record = eventually(lambda: record(root / "bootstrap"))
                groups.append(int(boot_record["process_id"]))
                saved = subprocess.check_output([helper, "--read-settings", str(settings)], env=env, text=True)
                assert saved.strip() == "0", "Connector startup must not change the saved preference"
                bootstrap.stdin.close()
                assert bootstrap.wait(timeout=15) == 0
                os.kill(int(boot_record["process_id"]), 0)
                os.killpg(int(boot_record["process_id"]), signal.SIGTERM)

                subprocess.run([helper, "--write-settings", str(settings), "1"], check=True, env=env)
                finite_env = dict(env, SIMXR_MCP_DISCOVERY_DIR=str(root / "finite"))
                smoke = subprocess.run([app, "--hidden", "--validation", "--smoke-frames", "8"], env=finite_env,
                                       stdout=log, stderr=subprocess.STDOUT, timeout=30)
                assert smoke.returncode == 0
                assert not (root / "finite").exists(), "Finite QA must not start MCP from saved settings"
                print("PASS saved startup, connector override and finite-QA isolation")
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                for group in groups:
                    try:
                        os.killpg(group, signal.SIGTERM)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    main()
