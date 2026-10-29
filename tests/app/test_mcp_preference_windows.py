#!/usr/bin/env python3
"""Check saved and connector-started MCP enablement on native Windows."""


import os
from pathlib import Path
import subprocess
import sys
import tempfile

from mcp_support import Client, discovery_record, eventually, kernel, process_handle


def main():
    app, connector, helper = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="simxr-pref-") as temporary:
        root = Path(temporary)
        settings = root / "settings"
        env = dict(os.environ, SIMXR_COMPANION_SETTINGS=str(settings), SIMXR_DATA_DIR=temporary,
                   PATH=str(Path(os.environ["SystemRoot"]) / "System32"))
        env.pop("SIMXR_MCP_TOKEN", None)
        env.pop("XR_RUNTIME_JSON", None)
        processes = []
        boot_handle = None
        with (root / "test.log").open("wb") as log:
            try:
                subprocess.run([helper, "--write-settings", str(settings), "1"], check=True, env=env)
                enabled_env = dict(env, SIMXR_MCP_DISCOVERY_DIR=str(root / "enabled"))
                app_process = subprocess.Popen([app, "--hidden"], env=enabled_env, stdout=log,
                                               stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                processes.append(app_process)

                initial = eventually(lambda: discovery_record(root / 'enabled'))
                assert initial["process_id"] == app_process.pid
                connector_process = subprocess.Popen([connector, "--app", app], env=enabled_env, stdin=subprocess.PIPE,
                                                     stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8",
                                                     bufsize=1, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                processes.append(connector_process)
                params = {"protocolVersion": "2025-11-25", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "1"}}
                assert "result" in Client(connector_process).rpc("initialize", params)
                assert discovery_record(root / 'enabled')["process_id"] == app_process.pid
                connector_process.stdin.close()
                assert connector_process.wait(timeout=15) == 0 and app_process.poll() is None
                app_process.terminate()
                app_process.wait(timeout=10)

                subprocess.run([helper, "--write-settings", str(settings), "0"], check=True, env=env)
                boot_env = dict(env, SIMXR_MCP_DISCOVERY_DIR=str(root / "bootstrap"))
                boot = subprocess.Popen([connector, "--app", app], env=boot_env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8",
                                        bufsize=1, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                processes.append(boot)
                assert "result" in Client(boot).rpc("initialize", params)
                boot_record = eventually(lambda: discovery_record(root / 'bootstrap'))
                boot_handle = process_handle(int(boot_record["process_id"]))
                saved = subprocess.check_output([helper, "--read-settings", str(settings)], env=env, text=True)
                assert saved.strip() == "0", "Connector startup must not change the saved preference"
                boot.stdin.close()
                assert boot.wait(timeout=15) == 0 and kernel.WaitForSingleObject(boot_handle, 0) == 0x102
                print("PASS Windows saved MCP startup and temporary connector override")
            except Exception:
                log.flush()
                print((root / "test.log").read_text(encoding="utf-8", errors="replace")[-4000:], file=sys.stderr)
                raise
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                if boot_handle:
                    if kernel.WaitForSingleObject(boot_handle, 0) == 0x102:
                        kernel.TerminateProcess(boot_handle, 1)
                        kernel.WaitForSingleObject(boot_handle, 10000)
                    kernel.CloseHandle(boot_handle)


if __name__ == "__main__":
    main()
