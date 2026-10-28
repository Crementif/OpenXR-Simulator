#!/usr/bin/env python3
"""Verify native Windows windowless connector, launcher and shutdown."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_mcp_connector import Client, eventually
from test_mcp_discovery_windows import kernel, process_handle


def main():
    app, connector, fixture = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="simxr-windowless-Ü-") as temporary:
        root = Path(temporary)
        discovery = root / "discovery"
        env = dict(os.environ, SIMXR_MCP_DISCOVERY_DIR=str(discovery), SIMXR_DATA_DIR=temporary,
                   SIMXR_COMPANION_SETTINGS=str(root / "settings"), SDL_VIDEODRIVER="simxr-unavailable-video-driver")
        env.pop("SIMXR_MCP_TOKEN", None)
        env.pop("XR_RUNTIME_JSON", None)
        handles = []
        with (root / "bridge.log").open("wb") as log:
            bridge = subprocess.Popen([connector, "--app", app, "--windowless"], env=env,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                      text=True, encoding="utf-8", bufsize=1,
                                      creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            try:
                client = Client(bridge)
                result = client.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                                   "clientInfo": {"name": "test", "version": "1"}})
                assert "result" in result, result
                client.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
                assert "simulator_shutdown" in {item["name"] for item in client.rpc("tools/list")["result"]["tools"]}

                def record():
                    files = list(discovery.glob("*.json"))
                    return json.loads(files[0].read_text(encoding="utf-8")) if len(files) == 1 else None

                app_handle = process_handle(int(eventually(record)["process_id"]))
                handles.append(app_handle)
                manifest = root / "manifest.json"
                manifest.write_text("{}", encoding="utf-8")
                executable = Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
                launched = client.tool("simulator_launch_application", {"executable": str(executable),
                    "runtime_manifest": str(manifest), "arguments": ["-NoProfile", "-Command", "Start-Sleep -Seconds 60"]})
                assert not launched["error"], launched
                application_id = launched["data"]["application_id"]

                def running():
                    status = client.tool("simulator_application_status", {"application_id": application_id})
                    assert not status["error"], status
                    if status["data"]["launch_state"] == "failed":
                        raise AssertionError(status)
                    return status["data"] if status["data"]["launch_state"] == "running" else None

                child_handle = process_handle(int(eventually(running)["process_id"]))
                handles.append(child_handle)
                marker = root / "fixture-ready"
                endpoint = rf"\\.\pipe\SimXR.Control.mcp.restart.{os.getpid()}"
                launched_fixture = client.tool("simulator_launch_application", {"executable": fixture,
                    "runtime_manifest": str(manifest), "arguments": [str(marker)],
                    "environment": {"SIMXR_CONTROL_SOCKET": endpoint}})
                assert not launched_fixture["error"], launched_fixture
                fixture_id = launched_fixture["data"]["application_id"]

                def fixture_status():
                    status = client.tool("simulator_application_status", {"application_id": fixture_id})
                    assert not status["error"], status
                    if status["data"]["launch_state"] == "failed":
                        raise AssertionError(status)
                    return status["data"] if status["data"]["session_running"] else None

                first = eventually(fixture_status)
                assert first["endpoint"] == endpoint
                opened = client.tool("debug_connect", {"application_id": fixture_id})
                assert not opened["error"], opened
                debug = opened["data"]["connection"]
                status = client.tool("debug_status", {"connection": debug})
                assert not status["error"] and status["data"]["running"], status
                acquired = client.tool("debug_acquire", {"connection": debug})
                assert not acquired["error"], acquired
                initial = client.tool("simulation_status", {"connection": debug})
                assert not initial["error"] and "document" in initial["data"], initial
                assert not client.tool("simulation_start_recording", {"connection": debug})["error"]
                transaction = {"connection": debug, "transaction_id": "9007199254740997", "frame": "999999",
                               "commands": [{"kind": "take_control", "channel": "gaze"},
                                            {"kind": "gaze", "yaw": 0.5}]}
                queued = client.tool("simulation_schedule", transaction)
                assert not queued["error"] and queued["data"]["phase"] == "queued", queued
                duplicate = client.tool("simulation_schedule", transaction)
                assert not duplicate["error"] and duplicate["data"]["transaction_id"] == transaction["transaction_id"]
                cancelled = client.tool("simulation_cancel", {"connection": debug,
                                        "transaction_id": transaction["transaction_id"]})
                assert not cancelled["error"] and cancelled["data"]["phase"] == "cancelled", cancelled
                assert not client.tool("simulation_release_control", {"connection": debug})["error"]
                assert not client.tool("simulation_stop_recording", {"connection": debug})["error"]
                assert not client.tool("debug_relinquish", {"connection": debug,
                    "lease": acquired["data"]["lease"]})["error"]
                first_handle = process_handle(int(first["process_id"]))
                handles.append(first_handle)
                eventually(marker.is_file)
                stale = client.tool("simulator_restart_application", {"application_id": fixture_id,
                                    "process_id": str(int(first["process_id"]) + 1)})
                assert stale["error"]
                accepted = client.tool("simulator_restart_application", {"application_id": fixture_id,
                                       "process_id": first["process_id"]})
                assert not accepted["error"] and accepted["data"]["accepted"]

                def replacement():
                    value = fixture_status()
                    return value if value and value["process_id"] != first["process_id"] else None

                second = eventually(replacement)
                assert second["endpoint"] == endpoint
                assert client.tool("debug_status", {"connection": debug})["error"]
                handles.append(process_handle(int(second["process_id"])))
                assert kernel.WaitForSingleObject(first_handle, 10000) == 0
                stale = client.tool("simulator_restart_application", {"application_id": fixture_id,
                                    "process_id": first["process_id"]})
                assert stale["error"]
                shutdown = client.tool("simulator_shutdown", {})
                assert not shutdown["error"] and shutdown["data"]["accepted"]
                assert kernel.WaitForSingleObject(app_handle, 15000) == 0
                assert kernel.WaitForSingleObject(child_handle, 0) == 0x102
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                print("PASS Windows custom-pipe restart, stale-ID rejection, shutdown and child survival")
            finally:
                if bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                for handle in handles:
                    if kernel.WaitForSingleObject(handle, 0) == 0x102:
                        kernel.TerminateProcess(handle, 1)
                    kernel.CloseHandle(handle)


if __name__ == "__main__":
    main()
