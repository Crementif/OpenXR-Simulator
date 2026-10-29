"""Platform helpers shared by packaging scenarios."""

import os
from pathlib import Path
import signal
import time


def reap_children():
    # The connector intentionally detaches the host, and the host can leave its
    # application running. As a subreaper, this test owns both on early failure.
    deadline = time.monotonic() + 10
    force = time.monotonic() + 5
    children = Path(f'/proc/self/task/{os.getpid()}/children')
    while True:
        while True:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                pid = 0
            if not pid:
                break
        pids = [int(value) for value in children.read_text().split()]
        if not pids:
            return
        for pid in pids:
            try:
                os.kill(pid, signal.SIGKILL if time.monotonic() >= force else signal.SIGTERM)
            except ProcessLookupError:
                pass
        if time.monotonic() >= deadline:
            raise RuntimeError(f'Failed to stop test processes: {pids}')
        time.sleep(.05)


def registry_value(root, view):
    import winreg

    try:
        with winreg.OpenKey(root, r"SOFTWARE\Khronos\OpenXR\1", 0, winreg.KEY_READ | view) as key:
            return winreg.QueryValueEx(key, "ActiveRuntime")
    except FileNotFoundError:
        return None
