#!/usr/bin/env python3
"""A peer that trickles headers must not hold an update worker indefinitely."""

import socket
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


def main():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        output = tempfile.TemporaryFile(mode="w+t", encoding="utf-8")
        marker_dir = tempfile.TemporaryDirectory(prefix="simxr-update-timeout-")
        marker = Path(marker_dir.name) / "result"
        target = "Z:" + str(marker).replace("/", "\\") if sys.argv[2:] else str(marker)
        process = subprocess.Popen([*sys.argv[2:], sys.argv[1], "--slow-http", str(listener.getsockname()[1]), target],
                                   stdout=output, stderr=output, text=True)
        stop = threading.Event()
        peer = None
        worker = None
        try:
            peer, _ = listener.accept()
            peer.settimeout(2)
            request = b""
            while b"\r\n\r\n" not in request:
                part = peer.recv(8192)
                assert part, "Update worker closed before sending its request"
                request += part
            peer.sendall(b"HTTP/1.1 200 OK\r\nX-Slow: ")

            def trickle():
                while not stop.wait(0.4):
                    try:
                        peer.sendall(b"a")
                    except OSError:
                        return

            worker = threading.Thread(target=trickle)
            worker.start()
            started = time.monotonic()
            process.wait(timeout=14)
            elapsed = time.monotonic() - started
            output.seek(0)
            log = output.read()
            assert process.returncode == 0 and marker.exists() and marker.read_text() == "PASS", log
            assert 2 <= elapsed < 12, elapsed
            stop.set()
            worker.join(timeout=3)
            try:
                remaining = peer.recv(1)
            except ConnectionResetError:
                remaining = b""
            assert remaining == b"", "Update worker retained the connection after shutdown"
        finally:
            stop.set()
            if worker:
                worker.join(timeout=3)
            if peer:
                peer.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            output.close()
            marker_dir.cleanup()
    print("PASS bounded WinHTTP slow-header shutdown")


if __name__ == "__main__":
    main()
