#!/usr/bin/env python3
"""Native Windows MCP discovery and independent-process integration."""

from concurrent.futures import ThreadPoolExecutor
import json
import msvcrt
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from mcp_support import Client, discovery_record, eventually, kernel, process_handle


def main():
    connector, fixture = (str(Path(value).resolve()) for value in sys.argv[1:3])
    options = sys.argv[3:]
    job_tests = next((str(Path(value).resolve()) for value in options if not value.startswith("--")), None)
    real_app = "--real-app" in options
    with tempfile.TemporaryDirectory(prefix="simxr-mcp-Ü-") as temporary:
        root = Path(temporary)
        app = root / "development ü build" / "test simulator.exe"
        app.parent.mkdir()
        shutil.copy2(fixture, app)
        discovery = root / "private discovery"
        env = dict(os.environ, SIMXR_MCP_DISCOVERY_DIR=str(discovery),
                   PATH=str(Path(os.environ["SystemRoot"]) / "System32"))
        env.pop("SIMXR_MCP_TOKEN", None)
        connectors = []
        app_handle = None
        game_handle = None
        manual = None
        with (root / "connector.log").open("wb") as log:
            try:
                for selected in (str(app), str(app).swapcase()):
                    process = subprocess.Popen([connector, "--app", selected], env=env, stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, stderr=log, text=True, encoding="utf-8",
                                               bufsize=1, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                    connectors.append(process)
                clients = [Client(process) for process in connectors]
                params = {"protocolVersion": "2025-11-25", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "1"}}
                with ThreadPoolExecutor(max_workers=2) as executor:
                    initialized = list(executor.map(lambda client: client.rpc("initialize", params), clients))
                assert all("result" in value for value in initialized), initialized
                for client in clients:
                    client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})

                initial = eventually(lambda: discovery_record(discovery))
                pid = int(initial["process_id"])
                app_handle = process_handle(pid)
                assert kernel.WaitForSingleObject(app_handle, 0) == 0x102
                assert "result" in clients[1].rpc("tools/list")
                if real_app:
                    marker = root / "child-credential.txt"
                    manifest = root / "override.json"
                    manifest.write_text("{}", encoding="utf-8")
                    powershell = Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
                    script = ("if (Test-Path Env:SIMXR_MCP_TOKEN) {$v='leaked'} else {$v='absent'}; "
                              f"[IO.File]::WriteAllText('{marker}', $v); Start-Sleep -Seconds 60")
                    launched = clients[1].tool("simulator_launch_application", {
                        "executable": str(powershell), "runtime_manifest": str(manifest),
                        "arguments": ["-NoProfile", "-NonInteractive", "-Command", script]})
                    assert not launched["error"], launched
                    app_id = launched["data"]["application_id"]

                    def running():
                        status = clients[1].tool("simulator_application_status", {"application_id": app_id})
                        assert not status["error"], status
                        if status["data"]["launch_state"] == "failed":
                            raise AssertionError(status["data"])
                        return status["data"] if status["data"]["launch_state"] == "running" else None

                    game_handle = process_handle(int(eventually(running)["process_id"]))
                    eventually(marker.is_file)
                    assert marker.read_text() == "absent"
                with (root / "wrong-handle").open("w+b") as wrong:
                    handle = msvcrt.get_osfhandle(wrong.fileno())
                    os.set_handle_inheritable(handle, True)
                    startup = subprocess.STARTUPINFO()
                    startup.lpAttributeList = {"handle_list": [handle]}
                    invalid = subprocess.run([str(app), "--mcp-bootstrap-handle", str(handle)], env=env,
                                             startupinfo=startup, close_fds=True, stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE, timeout=10)
                    assert invalid.returncode != 0 and b"Invalid MCP bootstrap reservation" in invalid.stderr
                assert discovery_record(discovery)["process_id"] == pid

                wrong_identity = dict(initial, index="0" if initial["index"] != "0" else "1")
                next(discovery.glob("*.json")).write_text(json.dumps(wrong_identity), encoding="utf-8")
                eventually(lambda: "error" in clients[1].rpc("tools/list"))
                next(discovery.glob("*.json")).write_text(json.dumps(initial), encoding="utf-8")
                clients[1].wait_notification("notifications/tools/list_changed")
                assert "result" in clients[1].rpc("tools/list")
                connectors[0].terminate()
                connectors[0].wait(timeout=10)
                assert kernel.WaitForSingleObject(app_handle, 0) == 0x102
                assert "result" in clients[1].rpc("tools/list")
                assert kernel.TerminateProcess(app_handle, 1)
                assert kernel.WaitForSingleObject(app_handle, 10000) == 0
                if game_handle:
                    assert kernel.WaitForSingleObject(game_handle, 0) == 0x102
                assert "error" in clients[1].rpc("tools/list")
                manual = subprocess.Popen([str(app), "--mcp"], env=env, stdin=subprocess.DEVNULL,
                                          stdout=log, stderr=subprocess.STDOUT,
                                          creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                eventually(lambda: discovery_record(discovery) and discovery_record(discovery)["process_id"] == manual.pid)
                clients[1].wait_notification("notifications/tools/list_changed")
                assert "result" in clients[1].rpc("tools/list")
                connectors[1].stdin.close()
                assert connectors[1].wait(timeout=15) == 0
                assert manual.poll() is None
                if game_handle:
                    assert kernel.WaitForSingleObject(game_handle, 0) == 0x102
                insecure = root / "insecure"
                insecure.mkdir()
                result = subprocess.run([str(Path(os.environ["SystemRoot"]) / "System32" / "icacls.exe"),
                                         str(insecure), "/grant", "*S-1-1-0:(OI)(CI)R"],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
                assert result.returncode == 0
                rejected = subprocess.run([connector, "--app", str(app)],
                                          env=dict(env, SIMXR_MCP_DISCOVERY_DIR=str(insecure)),
                                          stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                          timeout=10)
                assert rejected.returncode != 0
                target = root / "reparse-target"
                target.mkdir()
                link = root / "reparse-link"
                try:
                    os.symlink(target, link, target_is_directory=True)
                except OSError:
                    print("SKIP reparse-point rejection: Windows did not permit creating a symbolic link")
                else:
                    rejected = subprocess.run([connector, "--app", str(app)],
                                              env=dict(env, SIMXR_MCP_DISCOVERY_DIR=str(link)),
                                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                              timeout=10)
                    assert rejected.returncode != 0 and b"reparse point" in rejected.stderr
                if job_tests:
                    job_result = subprocess.run([job_tests, connector, str(app), str(root)], env=env,
                                                capture_output=True, timeout=60)
                    assert job_result.returncode == 0, (job_result.stdout, job_result.stderr)
                    print(job_result.stdout.decode("utf-8", errors="replace").strip())
                    assert manual.poll() is None
                print("PASS Windows discovery, ACL rejection, handle identity, Unicode paths, reconnect and independent app")
            except Exception:
                log.flush()
                print((root / "connector.log").read_text(encoding="utf-8", errors="replace")[-4000:], file=sys.stderr)
                raise
            finally:
                for process in connectors:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                if manual and manual.poll() is None:
                    manual.kill()
                    manual.wait()
                if app_handle:
                    if kernel.WaitForSingleObject(app_handle, 0) == 0x102:
                        kernel.TerminateProcess(app_handle, 1)
                        kernel.WaitForSingleObject(app_handle, 10000)
                    kernel.CloseHandle(app_handle)
                if game_handle:
                    if kernel.WaitForSingleObject(game_handle, 0) == 0x102:
                        kernel.TerminateProcess(game_handle, 1)
                        kernel.WaitForSingleObject(game_handle, 10000)
                    kernel.CloseHandle(game_handle)


if __name__ == "__main__":
    main()
