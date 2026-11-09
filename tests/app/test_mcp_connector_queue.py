#!/usr/bin/env python3
"""Verify queued connector cancellation without creating a target process."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import queue
import socket
import subprocess
import sys
import threading
import time


class State:
    def __init__(self):
        self.started = queue.Queue()
        self.cancelled = queue.Queue()
        self.release = threading.Event()
        self.notice_started = threading.Event()
        self.notice_release = threading.Event()
        self.long_release = threading.Event()
        self.stop_release = threading.Event()
        self.names = []
        self.keys = {}
        self.lock = threading.Lock()


class Server(ThreadingHTTPServer):
    request_queue_size = 32


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        message = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = message["method"]
        headers = {}
        if method == "initialize":
            headers["MCP-Session-Id"] = "test-session"
            result = {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                      "serverInfo": {"name": "test", "version": "1"}}
        elif method == "tools/call":
            name = message["params"]["name"]
            with self.server.state.lock:
                self.server.state.names.append(name)
                self.server.state.keys[message["id"]] = self.headers.get("X-Simxr-Cancellation-Key")
            if name == "interrupted":
                self.connection.shutdown(socket.SHUT_RDWR)
                self.close_connection = True
                return
            if name == 'chunked':
                body = json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': {'content': []}}).encode()
                self.send_response(200)
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                for start in range(0, len(body), 7):
                    chunk = body[start:start + 7]
                    self.wfile.write(f'{len(chunk):x}\r\n'.encode() + chunk + b'\r\n')
                    self.wfile.flush()
                self.wfile.write(b'0\r\nX-Test: done\r\n\r\n')
                return
            self.server.state.started.put(name)
            if name == 'long':
                self.server.state.long_release.wait(timeout=20)
            elif name == 'stop-block':
                self.server.state.stop_release.wait(timeout=40)
            else:
                self.server.state.release.wait(timeout=5)
            result = {"content": [{"type": "text", "text": "ok"}]}
        elif "id" not in message:
            if method == "notifications/cancelled":
                self.server.state.cancelled.put((message["params"]["requestId"],
                                                 self.headers.get("X-Simxr-Cancellation-Key")))
            if method == "notifications/test-block":
                self.server.state.notice_started.set()
                self.server.state.notice_release.wait(timeout=5)
            self.send_response(202)
            self.end_headers()
            return
        else:
            result = {}
        body = json.dumps({"jsonrpc": "2.0", "id": message["id"], "result": result}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def do_DELETE(self):
        self.send_response(204)
        self.end_headers()


def main():
    state = State()
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    server = None
    env = dict(os.environ, SIMXR_MCP_TOKEN="a" * 64)
    process = subprocess.Popen([sys.argv[1], "--port", str(port)], env=env, text=True,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=1)
    incoming = queue.Queue()

    def read():
        for line in process.stdout:
            incoming.put(json.loads(line))

    threading.Thread(target=read, daemon=True).start()

    def send(message):
        process.stdin.write(json.dumps(message) + "\n")
        process.stdin.flush()

    try:
        initialize = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}}
        send(initialize)
        initialized = incoming.get(timeout=5)
        assert initialized["id"] == 1 and "result" in initialized, "Initialization must not wait for the app"
        send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "early", "arguments": {}}})
        early = incoming.get(timeout=5)
        assert early["id"] == 2 and "error" in early, "An unavailable app must produce a bounded call error"
        server = Server(("127.0.0.1", port), Handler)
        server.state = state
        threading.Thread(target=server.serve_forever, daemon=True).start()
        changed = incoming.get(timeout=10)
        assert changed.get("method") == "notifications/tools/list_changed", changed
        for identity in range(10, 18):
            send({"jsonrpc": "2.0", "id": identity, "method": "tools/call",
                  "params": {"name": "block", "arguments": {}}})
        for _ in range(8):
            assert state.started.get(timeout=5) == "block"
        send({"jsonrpc": "2.0", "id": 99, "method": "tools/call",
              "params": {"name": "must-not-run", "arguments": {}}})
        send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 99}})
        send({"jsonrpc": "2.0", "method": "notifications/test-block"})
        assert state.notice_started.wait(timeout=2)
        for _ in range(33):
            send({"jsonrpc": "2.0", "method": "notifications/test-block"})
        send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": 10}})
        time.sleep(.2)
        state.notice_release.set()
        cancelled_id, key = state.cancelled.get(timeout=4)
        assert cancelled_id == 10 and len(key) == 64 and key == state.keys[10]
        time.sleep(.1)
        state.release.set()
        state.notice_release.set()
        received = {incoming.get(timeout=5)["id"] for _ in range(8)}
        assert received == set(range(10, 18)), received
        time.sleep(.2)
        with state.lock:
            assert "must-not-run" not in state.names, state.names
        assert incoming.empty(), "Cancelled queued request must not produce a response"

        send({"jsonrpc": "2.0", "id": 101, "method": "tools/call",
              "params": {"name": "long", "arguments": {}}})
        assert state.started.get(timeout=5) == 'long'
        time.sleep(7.5)
        assert incoming.empty(), 'A valid long call must not time out at seven seconds'
        state.long_release.set()
        response = incoming.get(timeout=5)
        assert response.get('id') == 101 and 'result' in response, response
        send({"jsonrpc": "2.0", "id": 103, "method": "tools/call",
              "params": {"name": "chunked", "arguments": {}}})
        response = incoming.get(timeout=5)
        assert response.get('id') == 103 and response.get('result', {}).get('content') == [], response

        send({"jsonrpc": "2.0", "id": 100, "method": "tools/call",
              "params": {"name": "interrupted", "arguments": {}}})
        deadline = time.monotonic() + 8
        while True:
            result = incoming.get(timeout=max(.01, deadline - time.monotonic()))
            if result.get("id") == 100:
                break
        assert "outcome may be unknown" in result["error"]["message"], result
        time.sleep(1.5)
        with state.lock:
            assert state.names.count("interrupted") == 1, state.names
        send({"jsonrpc": "2.0", "id": 102, "method": "tools/call",
              "params": {"name": "stop-block", "arguments": {}}})
        assert state.started.get(timeout=5) == 'stop-block'
        process.stdin.close()
        assert process.wait(timeout=5) == 0
        print("PASS queued cancellation, long calls and prompt connector shutdown")
    finally:
        state.release.set()
        state.long_release.set()
        state.stop_release.set()
        if process.poll() is None:
            process.kill()
            process.wait()
        errors = process.stderr.read()
        if errors:
            print(errors, file=sys.stderr)
        if server:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
