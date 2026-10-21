"""Owned legacy artifact namespace and bounded atomic filesystem access."""

from dataclasses import dataclass
from enum import Enum
import ctypes
import json
import os
from pathlib import Path
import re
import threading
import uuid


class CaptureFailed(RuntimeError):
    """An explicitly requested runtime capture completed with a failure."""


class Artifact(Enum):
    SETTINGS = "settings.json"
    STATUS = "runtime_status.json"
    FRAME_INFO = "frame_info.json"
    PROJECTION = "projection_log.json"
    FLICKER = "flicker_status.json"
    UI_FLICKER = "ui_flicker_status.json"
    SCREENSHOT = "screenshot_status.json"


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
    """Own one configured directory; never redirect an in-flight workflow.

    Command publication is atomic and uses a unique temporary per call. Close
    revokes new publications; the application drains admitted calls before close.
    Artifact readers never follow an incident outside this directory.
    """

    SCREENSHOTS = (
        "screenshot.png",
        "screenshot.bmp",
        "screenshot.jpg",
        "screenshot.jpeg",
    )
    MAX_IMAGE_BYTES = 512 * 1024 * 1024
    MAX_DOCUMENT_BYTES = 1024 * 1024
    MAX_LOG_BYTES = 64 * 1024 * 1024

    def __init__(self, directory, *, platform=None):
        self._platform = os.name if platform is None else platform
        self._directory = Path(directory)
        self._lock = threading.Lock()
        self._closed = False

    @property
    def directory(self):
        return self._directory

    def close(self):
        with self._lock:
            self._closed = True

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

    def publish(self, name, payload):
        encoded = json.dumps(payload, allow_nan=False).encode("utf-8")
        if len(encoded) > 64 * 1024:
            raise ValueError("Compatibility command exceeds 64 KiB")
        self.publish_bytes(name, encoded)

    def publish_bytes(self, name, encoded):
        if len(encoded) > 64 * 1024:
            raise ValueError("Compatibility command exceeds 64 KiB")
        destination = self._local(name)
        temporary = destination.with_name(
            destination.name + ".mcp." + uuid.uuid4().hex + ".tmp"
        )
        with self._lock:
            if self._closed:
                raise RuntimeError("Runtime artifact access closed")
        self._directory.mkdir(parents=True, exist_ok=True)
        try:
            with temporary.open("wb") as output:
                output.write(encoded)
            with self._lock:
                if self._closed:
                    raise RuntimeError("Runtime artifact access closed")
                temporary.replace(destination)
        finally:
            temporary.unlink(missing_ok=True)

    def remove(self, name):
        self._local(name).unlink(missing_ok=True)

    def _screenshot_names(self, layer):
        if layer == "quad" and self._platform == "nt":
            # Windows publishes the isolated quad separately. Do not consume a
            # composite file that becomes visible before the side quad does.
            return ("screenshot_quad.bmp",)
        # Linux publishes its requested layer through the ordinary filename.
        return self.SCREENSHOTS

    def clear_screenshots(self, layer="all"):
        for name in self._screenshot_names(layer):
            self.remove(name)
        self.remove(Artifact.SCREENSHOT.value)

    def screenshot(self, requested_at, layer="all"):
        status_path = self._local(Artifact.SCREENSHOT.value)
        try:
            status_fresh = status_path.stat().st_mtime >= requested_at
        except OSError:
            status_fresh = False
        if status_fresh:
            status = self.read_json(Artifact.SCREENSHOT, {})
            if (
                isinstance(status, dict)
                and status.get("layer") == layer
                and status.get("success") is False
            ):
                reason = str(status.get("error", "unknown failure"))[:2048]
                raise CaptureFailed(f"Runtime capture failed: {reason}")
        for name in self._screenshot_names(layer):
            path = self._local(name)
            try:
                if path.stat().st_mtime < requested_at - 0.5:
                    continue
                return self._read_bytes(path, self.MAX_IMAGE_BYTES)
            except OSError:
                continue
        return None

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

    def preview_count(self, directory):
        return sum(1 for _ in directory.glob("frame*_preview.bmp"))
