#!/usr/bin/env python3
"""Exercise the app-owned HTTP MCP endpoint and its real launcher."""

import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def request(port, body, token, *, host=None, origin=None, version=None, session=None, capture=None, method="POST"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    if host is not None:
        headers["Host"] = host
    if origin is not None:
        headers["Origin"] = origin
    if version is not None:
        headers["MCP-Protocol-Version"] = version
    if session is not None:
        headers["MCP-Session-Id"] = session
    connection.request(method, "/mcp", body=body, headers=headers)
    response = connection.getresponse()
    data = response.read()
    content_type = response.getheader("Content-Type", "")
    if capture is not None:
        capture.append(response.getheader("MCP-Session-Id"))
    connection.close()
    if not data:
        return response.status, None
    if content_type.startswith("application/json"):
        return response.status, json.loads(data)
    return response.status, data.decode("utf-8", errors="replace")


def rpc(port, token, method, params=None, identity=1, session=None):
    body = {"jsonrpc": "2.0", "id": identity, "method": method}
    if params is not None:
        body["params"] = params
    status, value = request(port, json.dumps(body), token, version="2025-11-25", session=session)
    assert status == 200 and value["id"] == identity and "result" in value, (status, value)
    return value["result"]


def tool(port, token, session, name, arguments):
    result = rpc(port, token, "tools/call", {"name": name, "arguments": arguments}, session=session)
    assert result["content"][0]["type"] == "text", result
    return result["isError"], json.loads(result["content"][0]["text"])


def eventually(check, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError("Timed out waiting for MCP application state")


def main():
    app, probe = (str(Path(value).resolve()) for value in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="simxr-mcp-http-") as temporary:
        directory = Path(temporary)
        token = "0123456789abcdef" * 4
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        env = dict(os.environ, SIMXR_MCP_TOKEN=token, SIMXR_DATA_DIR=temporary,
                   SIMXR_COMPANION_SETTINGS=str(directory / "settings"), SIMXR_DETERMINISTIC="0")
        env.pop("XR_RUNTIME_JSON", None)
        output = directory / "app.log"
        with output.open("wb") as log:
            process = subprocess.Popen([app, "--hidden", "--validation", "--mcp-http", str(port)], env=env,
                                       stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                def ready():
                    if process.poll() is not None:
                        raise AssertionError("App exited before MCP startup: " + output.read_text(errors="replace")[-4000:])
                    try:
                        return request(port, "{}", None)[0] == 401
                    except OSError:
                        return False

                eventually(ready)
                assert request(port, "{}", "wrong")[0] == 401
                assert request(port, "{}", token, host="attacker.example")[0] == 403
                assert request(port, "{}", token, origin="https://attacker.example")[0] == 403
                assert request(port, "{}", token, version="2099-01-01")[0] == 400
                assert request(port, "{", token)[0] == 400
                initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                    "protocolVersion": "2025-11-25", "capabilities": {},
                    "clientInfo": {"name": "test", "version": "1"}}}
                captured = []
                status, init = request(port, json.dumps(initialize), token, capture=captured)
                assert status == 200 and init["result"]["protocolVersion"] == "2025-11-25"
                session = captured[0]
                assert session and len(session) == 64
                second = []
                assert request(port, json.dumps(initialize), token, capture=second)[0] == 200
                assert second[0] and second[0] != session
                listing = json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
                assert request(port, listing, token)[0] == 400
                assert request(port, listing, token, session="unknown")[0] == 404
                assert "error" in request(port, listing, token, session=session)[1]
                initialized = json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"})
                assert request(port, initialized, token, session=session)[0] == 202
                assert request(port, initialized, token, session=second[0])[0] == 202
                assert request(port, listing, token, version="2025-06-18", session=session)[0] == 400
                names = {entry["name"] for entry in rpc(port, token, "tools/list", session=session)["tools"]}
                assert {"simulator_launch_application", "simulator_list_applications",
                        "simulator_application_status", "simulator_stop_application",
                        "simulator_restart_application"} <= names
                assert tool(port, token, session, "simulator_list_applications", {}) == (False, {"applications": []})

                marker = directory / "credential-check"
                script = 'if [ "${SIMXR_MCP_TOKEN+x}" ]; then printf leaked > "$1"; else printf absent > "$1"; fi'
                failed, child = tool(port, token, session, "simulator_launch_application",
                                     {"executable": "/bin/sh", "arguments": ["-c", script, "sh", str(marker)]})
                assert not failed, child
                eventually(marker.is_file)
                assert marker.read_text() == "absent"

                failed, launched = tool(port, token, session, "simulator_launch_application",
                                        {"executable": probe, "environment": {"SIMXR_PROBE_FRAMES": "120000"}})
                assert not failed, launched
                app_id = launched["application_id"]
                assert len(app_id.split(":")[0]) == 64
                other_instance = ("0" if app_id[0] != "0" else "1") + app_id[1:]
                error, stale = tool(port, token, session, "simulator_application_status",
                                    {"application_id": other_instance})
                assert error and "different simulator instance" in stale["error"]

                def connected():
                    error, state = tool(port, token, session, "simulator_application_status", {"application_id": app_id})
                    assert not error, state
                    if state["launch_state"] == "failed":
                        raise AssertionError(state)
                    return state if state["connected"] and state["session_running"] else None

                state = eventually(connected)
                assert state["session_id"] != "0" and state["generation"] != "0", state
                original_pid = state["process_id"]
                error, stale = tool(port, token, session, "simulator_restart_application",
                                    {"application_id": app_id, "process_id": str(int(original_pid) + 1)})
                assert error, stale
                error, accepted = tool(port, token, session, "simulator_restart_application",
                                       {"application_id": app_id, "process_id": original_pid})
                assert not error and accepted["accepted"], accepted

                def reconnected():
                    result = connected()
                    return result if result and result["process_id"] != original_pid else None

                replacement = eventually(reconnected, seconds=30)
                assert replacement["session_id"] != "0" and not replacement["restart_pending"]
                assert replacement["endpoint"] != state["endpoint"]
                error, stale = tool(port, token, session, "simulator_restart_application",
                                    {"application_id": app_id, "process_id": original_pid})
                assert error, stale
                error, listed = tool(port, token, session, "simulator_list_applications", {})
                assert not error and any(item["application_id"] == app_id for item in listed["applications"])
                error, invalid = tool(port, token, session, "simulator_launch_application", {"executable": "relative"})
                assert error and "absolute" in invalid["error"]
                error, stopped = tool(port, token, session, "simulator_stop_application", {"application_id": app_id})
                assert not error and stopped["accepted"]

                def exited():
                    error, status = tool(port, token, session, "simulator_application_status", {"application_id": app_id})
                    assert not error, status
                    return status["launch_state"] == "exited"

                eventually(exited)
                error, stopped = tool(port, token, session, "simulator_stop_application", {"application_id": app_id})
                assert error
                assert request(port, "", token, session=session, method="DELETE")[0] == 204
                assert request(port, listing, token, session=session)[0] == 404
                assert request(port, listing, token, session=second[0])[0] == 200
                assert request(port, "", token, session=second[0], method="DELETE")[0] == 204

                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=10)
                with socket.socket() as reservation:
                    reservation.bind(("127.0.0.1", 0))
                    port = reservation.getsockname()[1]
                process = subprocess.Popen([app, "--hidden", "--validation", "--mcp-http", str(port)], env=env,
                                           stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                eventually(ready)
                restarted = []
                assert request(port, json.dumps(initialize), token, capture=restarted)[0] == 200
                assert request(port, initialized, token, session=restarted[0])[0] == 202
                error, stale = tool(port, token, restarted[0], "simulator_application_status",
                                    {"application_id": app_id})
                assert error and "different simulator instance" in stale["error"]
                print("PASS HTTP MCP authentication, lifecycle, credential isolation and real application launch")
            finally:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
