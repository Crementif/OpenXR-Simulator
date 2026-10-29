"""Shared support for native MCP integration tests."""

from contextlib import ExitStack
import json
import os
from pathlib import Path
import queue
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
    kernel.TerminateProcess.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)

    def process_handle(pid):
        handle = kernel.OpenProcess(0x00100000 | 0x0001 | 0x1000, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

class Client:
    def __init__(self, process):
        self.process = process
        self.incoming = queue.Queue()
        self.notifications = []
        self.next_id = 0
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.process.stdout:
            self.incoming.put(json.loads(line))

    def send(self, message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def initialize(self, name="test"):
        result = self.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                       "clientInfo": {"name": name, "version": "1"}})
        assert "result" in result, result
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return result

    def rpc(self, method, params=None):
        self.next_id += 1
        identity = self.next_id
        message = {"jsonrpc": "2.0", "id": identity, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        deadline = time.monotonic() + 20
        while True:
            item = self.incoming.get(timeout=max(.01, deadline - time.monotonic()))
            if item.get("id") == identity:
                return item
            assert "id" not in item, item
            self.notifications.append(item)

    def tool(self, name, arguments):
        result = self.rpc("tools/call", {"name": name, "arguments": arguments})
        if "error" in result:
            return result
        result = result["result"]
        return {"error": result.get("isError", False), "data": json.loads(result["content"][0]["text"])}

    def wait_notification(self, method):
        deadline = time.monotonic() + 15
        while True:
            for index, item in enumerate(self.notifications):
                if item.get("method") == method:
                    return self.notifications.pop(index)
            item = self.incoming.get(timeout=max(.01, deadline - time.monotonic()))
            assert "id" not in item, item
            self.notifications.append(item)


def eventually(check, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError("Timed out waiting for simulator state")


def discovery_record(directory):
    files = list(directory.glob("*.json"))
    return json.loads(files[0].read_text(encoding="utf-8")) if len(files) == 1 else None


def stop_process(process, group=False):
    if process.poll() is None:
        try:
            if group and os.name != "nt":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass
    process.wait(timeout=10)
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream and not stream.closed:
            stream.close()


def stop_tracked_process(handle):
    if os.name == "nt":
        try:
            if kernel.WaitForSingleObject(handle, 0) == 0x102:
                if not kernel.TerminateProcess(handle, 1):
                    raise ctypes.WinError(ctypes.get_last_error())
                assert kernel.WaitForSingleObject(handle, 10000) == 0
        finally:
            kernel.CloseHandle(handle)
    else:
        try:
            signal.pidfd_send_signal(handle, signal.SIGTERM)
            if not select.select([handle], [], [], 5)[0]:
                signal.pidfd_send_signal(handle, signal.SIGKILL)
                assert select.select([handle], [], [], 5)[0], "Process did not exit"
        except ProcessLookupError:
            pass
        finally:
            os.close(handle)


class McpTest:
    def __init__(self, prefix, *, windowless=False):
        self.resources = ExitStack()
        self.root = Path(self.resources.enter_context(tempfile.TemporaryDirectory(prefix=prefix)))
        self.env = dict(os.environ, SIMXR_DATA_DIR=str(self.root),
                        SIMXR_MCP_DISCOVERY_DIR=str(self.root / "discovery"),
                        SIMXR_COMPANION_SETTINGS=str(self.root / "settings"))
        for key in ("XR_RUNTIME_JSON", "SIMXR_MCP_TOKEN", "SIMXR_CONTROL_SOCKET"):
            self.env.pop(key, None)
        if windowless:
            self.env.update(SDL_VIDEODRIVER="simxr-unavailable-video-driver", SIMXR_DETERMINISTIC="0")
            for key in ("DISPLAY", "WAYLAND_DISPLAY"):
                self.env.pop(key, None)
        self.handles = {}
        self.logs = []

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        try:
            if kind is not None:
                for log in self.logs:
                    log.flush()
                    print(Path(log.name).read_text(encoding="utf-8", errors="replace")[-4000:], file=sys.stderr)
        finally:
            self.resources.close()

    def log(self, name):
        log = self.resources.enter_context((self.root / name).open("wb"))
        self.logs.append(log)
        return log

    def start(self, command, **options):
        options.setdefault("env", self.env)
        process = subprocess.Popen(command, **options)
        self.resources.callback(stop_process, process, options.get("start_new_session", False))
        return process

    def bridge(self, connector, *arguments, log, env=None):
        options = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt"
                   else {"start_new_session": True})
        return self.start([connector, *arguments], env=self.env if env is None else env,
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                          text=True, encoding="utf-8", bufsize=1, **options)

    def connect(self, connector, *arguments, log, name="test"):
        client = Client(self.bridge(connector, *arguments, log=log))
        client.initialize(name)
        return client

    def track(self, pid):
        pid = int(pid)
        if pid not in self.handles:
            handle = process_handle(pid) if os.name == "nt" else os.pidfd_open(pid)
            self.handles[pid] = handle
            self.resources.callback(stop_tracked_process, handle)
        return self.handles[pid]

    def discover(self):
        record = eventually(lambda: discovery_record(Path(self.env["SIMXR_MCP_DISCOVERY_DIR"])))
        self.track(record["process_id"])
        return record

    def connected(self, client, application, timeout=30):
        def ready():
            status = client.tool("simulator_application_status", {"application_id": application})
            assert not status["error"] and status["data"]["launch_state"] not in ("failed", "exited"), status
            if int(status["data"].get("process_id", 0)) > 0:
                self.track(status["data"]["process_id"])
            return status["data"] if status["data"]["session_running"] else None

        return eventually(ready, timeout=timeout)

