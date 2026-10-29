#!/usr/bin/env python3
"""Exercise the native stdio bridge against real simulator processes."""

import http.client
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile

from mcp_support import Client, eventually

def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="simxr-mcp-connector-") as temporary:
        directory = Path(temporary)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        token = "0123456789abcdef" * 4
        env = dict(os.environ, SIMXR_MCP_TOKEN=token, SIMXR_DATA_DIR=temporary,
                   SIMXR_COMPANION_SETTINGS=str(directory / "settings"), SIMXR_DETERMINISTIC="0")
        env.pop("XR_RUNTIME_JSON", None)
        groups = []
        with (directory / "app.log").open("wb") as app_log, (directory / "connector.log").open("wb") as bridge_log:
            def start_app():
                process = subprocess.Popen([app, "--hidden", "--validation", "--mcp-http", str(port)], env=env,
                                           stdout=app_log, stderr=subprocess.STDOUT, start_new_session=True)
                groups.append(process.pid)

                def ready():
                    if process.poll() is not None:
                        raise AssertionError("App exited: " + (directory / "app.log").read_text(errors="replace")[-4000:])
                    try:
                        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
                        connection.request("POST", "/mcp", body="{}", headers={"Content-Type": "application/json"})
                        response = connection.getresponse()
                        response.read()
                        connection.close()
                        return response.status == 401
                    except OSError:
                        return False

                eventually(ready)
                return process

            server = None
            bridge = None
            try:
                server = start_app()
                bridge = subprocess.Popen([connector, "--port", str(port)], env=env, stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=bridge_log, text=True, bufsize=1)
                client = Client(bridge)
                init = client.initialize('test')
                assert init["result"]["capabilities"]["tools"]["listChanged"]

                names = {entry["name"] for entry in client.rpc("tools/list")["result"]["tools"]}
                assert "simulator_launch_application" in names
                launched = client.tool("simulator_launch_application", {"executable": probe,
                                      "environment": {"SIMXR_PROBE_FRAMES": "120000"}})
                assert not launched["error"], launched
                app_id = launched["data"]["application_id"]

                def connected():
                    status = client.tool("simulator_application_status", {"application_id": app_id})
                    assert not status["error"], status
                    return status["data"] if status["data"]["session_running"] else None

                state = eventually(connected)
                child_pid = int(state["process_id"])
                expect_luau = os.environ.get("SIMXR_EXPECT_MCP_LUAU", "1") == "1"
                assert ("simulator_luau_run" in names) == expect_luau
                if expect_luau:
                    script = client.tool("simulator_luau_run", {"application_id": app_id,
                                         "source": "simxr.checkpoint('visible run', true)"})
                    assert not script["error"], script
                    run = script["data"]["run_id"]

                    def script_finished():
                        status = client.tool("simulator_luau_status", {"application_id": app_id, "run_id": run})
                        assert not status["error"], status
                        return status["data"] if status["data"]["phase"] not in ("running", "paused") else None

                    result = eventually(script_finished)
                    assert result["phase"] == "complete" and result["checkpoints"][0]["passed"], result
                os.kill(server.pid, signal.SIGTERM)
                server.wait(timeout=10)
                os.kill(child_pid, 0)
                unavailable = client.rpc("tools/list")
                assert "error" in unavailable, unavailable
                assert bridge.poll() is None

                server = start_app()
                client.wait_notification("notifications/tools/list_changed")
                names = {entry["name"] for entry in client.rpc("tools/list")["result"]["tools"]}
                assert "simulator_launch_application" in names
                stale = client.tool("simulator_application_status", {"application_id": app_id})
                assert stale["error"] and "different simulator instance" in stale["data"]["error"]
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                assert server.poll() is None
                os.kill(child_pid, 0)
                print("PASS native stdio bridge, app restart, stale identity and independent game survival")
            finally:
                if bridge and bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                for group in groups:
                    try:
                        os.killpg(group, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                if server:
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()


if __name__ == "__main__":
    main()
