#!/usr/bin/env python3
"""Exercise cold start, competing connectors and discovery after an app restart."""

from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time

from mcp_support import Client, discovery_record, eventually


def check_failed_start(connector, root, env):
    marker = root / "startup-attempts"
    failing = root / "failing-simulator"
    failing.write_text(f"#!/bin/sh\nprintf x >> '{marker}'\nexit 23\n")
    failing.chmod(0o700)
    child_env = dict(env, SIMXR_MCP_DISCOVERY_DIR=str(root / "failed-discovery"))
    process = subprocess.Popen([connector, "--app", str(failing)], env=child_env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
                               start_new_session=True)
    try:
        client = Client(process)
        result = client.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                           "clientInfo": {"name": "test", "version": "1"}})
        assert "exit code 23" in result["error"]["message"], result
        time.sleep(1.5)
        assert marker.read_text() == "x", "The connector must not repeatedly respawn a failed app"
        process.stdin.close()
        assert process.wait(timeout=10) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()


def main():
    source_app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="simxr-mcp-discovery-") as temporary:
        root = Path(temporary)
        build = root / "development build"
        build.mkdir()
        app = str(build / "simxr_companion")
        shutil.copy2(source_app, app)
        for name in ("openxr_simulator.json", "libopenxr_simulator.so", "openxr-simulator.bmp"):
            (build / name).symlink_to(Path(source_app).parent / name)
        empty_path = root / "empty-path"
        empty_path.mkdir()
        discovery = root / "discovery"
        env = dict(os.environ, PATH=str(empty_path), SIMXR_MCP_DISCOVERY_DIR=str(discovery),
                   SIMXR_DATA_DIR=temporary, SIMXR_COMPANION_SETTINGS=str(root / "settings"),
                   SIMXR_DETERMINISTIC="0")
        env.pop("XR_RUNTIME_JSON", None)
        env.pop("SIMXR_MCP_TOKEN", None)
        check_failed_start(connector, root, env)
        insecure = root / "insecure-discovery"
        insecure.mkdir(mode=0o755)
        insecure.chmod(0o755)
        rejected = subprocess.run([connector, "--app", app], env=dict(env, SIMXR_MCP_DISCOVERY_DIR=str(insecure)),
                                  stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
        assert rejected.returncode != 0 and b"inaccessible to other users" in rejected.stderr
        groups = []
        processes = []
        manual = None
        with (root / "connectors.log").open("wb") as log:
            try:
                for _ in range(2):
                    process = subprocess.Popen([connector, "--app", app], env=env, stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
                                               start_new_session=True)
                    processes.append(process)
                    groups.append(process.pid)
                clients = [Client(process) for process in processes]
                params = {"protocolVersion": "2025-11-25", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "1"}}
                with ThreadPoolExecutor(max_workers=2) as executor:
                    initializations = list(executor.map(lambda client: client.rpc("initialize", params), clients))
                assert all("result" in result for result in initializations), initializations
                for client in clients:
                    client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

                initial = eventually(lambda: discovery_record(discovery))
                app_pid = int(initial["process_id"])
                groups.append(app_pid)
                assert os.getsid(app_pid) == app_pid and os.getpgid(app_pid) == app_pid
                assert initial["executable"] == app
                assert stat.S_IMODE(discovery.stat().st_mode) == 0o700
                assert stat.S_IMODE(next(discovery.glob("*.json")).stat().st_mode) == 0o600
                assert stat.S_IMODE(next(discovery.glob("*.lock")).stat().st_mode) == 0o600
                marker = root / "credential-check"
                script = 'if [ "${SIMXR_MCP_TOKEN+x}" ]; then printf leaked > "$1"; else printf absent > "$1"; fi'
                check = clients[0].tool("simulator_launch_application", {"executable": "/bin/sh",
                                        "arguments": ["-c", script, "sh", str(marker)]})
                assert not check["error"], check
                eventually(marker.is_file)
                assert marker.read_text() == "absent"
                launched = clients[0].tool("simulator_launch_application", {"executable": probe,
                                             "environment": {"SIMXR_PROBE_FRAMES": "120000"}})
                assert not launched["error"], launched
                app_id = launched["data"]["application_id"]

                def connected():
                    result = clients[1].tool("simulator_application_status", {"application_id": app_id})
                    assert not result["error"], result
                    return result["data"] if result["data"]["session_running"] else None

                child_pid = int(eventually(connected)["process_id"])
                processes[0].stdin.close()
                assert processes[0].wait(timeout=15) == 0
                assert not clients[1].tool("simulator_application_status", {"application_id": app_id})["error"]
                replacement = build / "rebuilt-simulator"
                shutil.copy2(app, replacement)
                os.replace(replacement, app)
                eventually(lambda: "error" in clients[1].rpc("tools/list"))
                os.kill(app_pid, signal.SIGTERM)

                def stopped():
                    try:
                        fields = Path(f"/proc/{app_pid}/stat").read_text().split()
                        return fields[2] == "Z"
                    except FileNotFoundError:
                        return True

                eventually(stopped)
                os.kill(child_pid, 0)
                assert "error" in clients[1].rpc("tools/list")
                manual = subprocess.Popen([app, "--hidden", "--mcp"], env=env, stdout=log,
                                          stderr=subprocess.STDOUT, start_new_session=True)
                groups.append(manual.pid)
                eventually(lambda: discovery_record(discovery) and discovery_record(discovery)["process_id"] == manual.pid)
                clients[1].wait_notification("notifications/tools/list_changed")
                names = {tool["name"] for tool in clients[1].rpc("tools/list")["result"]["tools"]}
                assert "simulator_launch_application" in names
                stale = clients[1].tool("simulator_application_status", {"application_id": app_id})
                assert stale["error"] and "different simulator instance" in stale["data"]["error"]
                os.killpg(processes[1].pid, signal.SIGTERM)
                assert processes[1].wait(timeout=15) == -signal.SIGTERM
                assert manual.poll() is None
                os.kill(child_pid, 0)
                print("PASS private discovery, concurrent cold start, dynamic reconnect and game survival")
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                for entry in Path("/proc").iterdir():
                    if not entry.name.isdigit():
                        continue
                    try:
                        arguments = (entry / "cmdline").read_bytes().split(b"\0")
                        pid = int(entry.name)
                        if arguments[0] == os.fsencode(app) and b"--mcp-bootstrap-fd" in arguments and os.getpgid(pid) == pid:
                            groups.append(pid)
                    except (FileNotFoundError, ProcessLookupError, PermissionError):
                        pass
                for group in groups:
                    try:
                        os.killpg(group, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                if manual:
                    try:
                        manual.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        manual.kill()
                        manual.wait()


if __name__ == "__main__":
    main()
