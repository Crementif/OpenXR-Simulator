"""Owned legacy artifact namespace and bounded atomic filesystem access."""

from dataclasses import dataclass
from enum import Enum
import ctypes
import json
import os
from pathlib import Path
import re


class CaptureFailed(RuntimeError):
    """An explicitly requested runtime capture completed with a failure."""


class Artifact(Enum):
    SETTINGS = "settings.json"
    STATUS = "runtime_status.json"
    FRAME_INFO = "frame_info.json"
    FLICKER = "flicker_status.json"
    UI_FLICKER = "ui_flicker_status.json"


@dataclass(frozen=True)
class LogSnapshot:
    path: Path
    text: str
    modified: float
    pid: int | None
    live: bool
    truncated: bool


def simulator_directory():
    if os.environ.get("SIMXR_DATA_DIR"):
        return Path(os.environ["SIMXR_DATA_DIR"])
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", "")) / "OpenXR-Simulator"
    return (
        Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
        / "OpenXR-Simulator"
    )


def _pid_alive(pid):
    if pid is None:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
    kernel = ctypes.windll.kernel32
    # HANDLE is pointer-sized; ctypes' default int result truncates Win64 handles.
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.GetExitCodeProcess.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_ulong),
    ]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        return (
            bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code)))
            and code.value == 259
        )
    finally:
        kernel.CloseHandle(handle)


class RuntimeArtifacts:
    """Read bounded artifacts from one configured runtime directory."""

    MAX_DOCUMENT_BYTES = 1024 * 1024
    MAX_LOG_BYTES = 64 * 1024 * 1024

    def __init__(self, directory):
        self._directory = Path(directory)

    @property
    def directory(self):
        return self._directory

    def _local(self, name):
        if (
            not isinstance(name, str)
            or not name
            or Path(name).name != name
            or name in (".", "..")
        ):
            raise ValueError("A local runtime artifact name is required")
        return self._directory / name

    def exists(self, artifact):
        return self._local(artifact.value).exists()

    def modified(self, artifact):
        return self._local(artifact.value).stat().st_mtime

    def read_json(self, artifact, default=None):
        try:
            text = self._read_bytes(
                self._local(artifact.value), self.MAX_DOCUMENT_BYTES
            )
            return json.loads(text)
        except (OSError, ValueError):
            return default

    @staticmethod
    def _read_bytes(path, maximum):
        with path.open("rb") as source:
            data = source.read(maximum + 1)
        if len(data) > maximum:
            raise ValueError(f"Runtime artifact exceeds its {maximum}-byte bound")
        return data

    def log_snapshot(self):
        candidates = []
        for path in self._directory.glob("openxr_simulator*.log"):
            try:
                candidates.append((path.stat().st_mtime, path))
            except OSError:
                continue
        if not candidates:
            return None
        modified, path = max(candidates)
        try:
            with path.open("rb") as source:
                source.seek(0, 2)
                size = source.tell()
                truncated = size > self.MAX_LOG_BYTES
                source.seek(max(0, size - self.MAX_LOG_BYTES))
                text = source.read(self.MAX_LOG_BYTES).decode("utf-8", errors="replace")
        except OSError:
            return None
        match = re.fullmatch(r"openxr_simulator\.(\d+)\.log", path.name)
        pid = int(match.group(1)) if match else None
        return LogSnapshot(path, text, modified, pid, _pid_alive(pid), truncated)

    def clear_logs(self):
        removed, held = [], []
        for path in self._directory.glob("openxr_simulator*.log"):
            try:
                path.unlink()
                removed.append(path.name)
            except OSError:
                held.append(path.name)
        return removed, held

    def incident_directory(self, value):
        path = Path(value).resolve()
        path.relative_to(self._directory.resolve())
        return path
