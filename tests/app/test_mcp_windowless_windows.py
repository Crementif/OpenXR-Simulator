#!/usr/bin/env python3
"""Verify native Windows windowless connector, launcher and shutdown."""


import os
from pathlib import Path
import sys

from mcp_support import Client, McpTest, eventually, kernel


def main():
    app, connector, fixture = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-windowless-Ü-', windowless=True) as test:
        root = test.root

        log = test.log('bridge.log')
        bridge = test.bridge(connector, "--app", app, "--windowless", log=log)
        client = Client(bridge)
        result = client.initialize('test')
        assert "result" in result, result

        assert "simulator_shutdown" in {item["name"] for item in client.rpc("tools/list")["result"]["tools"]}

        app_handle = test.track(test.discover()['process_id'])

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

        child_handle = test.track(int(eventually(running)["process_id"]))

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
        started_recording = client.tool("simulation_start_recording", {"connection": debug, "drain": True})
        assert not started_recording["error"], started_recording
        recording_id = started_recording["data"]["document"]["recording_id"]
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
        assert not client.tool("simulation_stop_recording", {"connection": debug, "recording_id": recording_id})["error"]
        assert not client.tool("debug_relinquish", {"connection": debug,
            "lease": acquired["data"]["lease"]})["error"]
        first_handle = test.track(int(first["process_id"]))

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
        test.track(int(second["process_id"]))
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


if __name__ == "__main__":
    main()
