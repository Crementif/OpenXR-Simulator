"""Exclusive legacy request lifetimes, interruptible waits, and CPU analysis."""

from contextlib import contextmanager
from io import BytesIO
import json
import math
import threading
import time
from image_analysis import (
    SCREENSHOT_JPEG_QUALITY,
    SCREENSHOT_MAX_WIDTH,
    build_flicker_contact_sheet,
    to_jpeg,
    validate_stereo_image,
)
from runtime_artifacts import Artifact
from retained_screenshot import RetainedScreenshot
from tool_results import ImageResult, TextResult


class WorkflowBusy(RuntimeError):
    pass


class WorkflowClosed(RuntimeError):
    pass


class RequestLane:
    """One shared runtime filename admits one workflow, never a waiting backlog."""

    def __init__(self):
        self._lock = threading.Lock()
        self._closed = threading.Event()

    @contextmanager
    def admit(self):
        if not self._lock.acquire(blocking=False):
            raise WorkflowBusy(
                "A request already owns this runtime artifact channel; retry explicitly"
            )
        try:
            if self._closed.is_set():
                raise WorkflowClosed("Runtime artifact workflow closed")
            yield
        finally:
            self._lock.release()

    def wait(self, seconds):
        if self._closed.wait(max(0.0, seconds)):
            raise WorkflowClosed("Runtime artifact workflow closed")

    def close(self):
        self._closed.set()


def _timeout(value):
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise ValueError("A finite nonnegative timeout is required")
    return value


class ProjectionWorkflow:
    def __init__(self, artifacts):
        self._artifacts = artifacts
        self._lane = RequestLane()

    def close(self):
        self._lane.close()

    def read(self, timeout=2.5):
        with self._lane.admit():
            self._artifacts.remove(Artifact.PROJECTION.value)
            self._artifacts.publish_bytes("projection_log_dump_request", b"")
            deadline = time.monotonic() + _timeout(timeout)
            while time.monotonic() < deadline:
                document = self._artifacts.read_json(Artifact.PROJECTION)
                if document is not None:
                    return document
                self._lane.wait(min(0.05, max(0.0, deadline - time.monotonic())))
            return None

    def tool(self, arguments):
        document = self.read()
        if document is None:
            return [
                TextResult(
                    "Projection log was not produced — is the simulator currently rendering frames?"
                )
            ]
        try:
            entries = document.get("entries", [])
            limit = int(arguments.get("max_entries", 20))
            tail = entries[-limit:] if len(entries) > limit else entries
            return [
                TextResult(json.dumps({"count": len(tail), "entries": tail}, indent=2))
            ]
        except Exception as error:
            return [TextResult(f"Failed to parse projection log: {error}")]


class CaptureWorkflow:
    """Own screenshot cleanup through analysis, across every tool using that file.

    Multi-frame stereo analysis keeps the same lane throughout the series. A
    second tool cannot erase its marker, steal its image, or silently enqueue
    another screenshot. Close interrupts waits without resubmitting a request.
    """

    def __init__(self, artifacts, projections):
        self._artifacts = artifacts
        self._projections = projections
        self._lane = RequestLane()
        self._retained = RetainedScreenshot(artifacts.directory)
        self._capture_metadata = None

    def close(self):
        self._lane.close()

    def disconnect(self):
        # The application owner calls this after admitted workers have joined.
        self._retained.close()

    def _capture(self, timeout, eye="both", layer="projection", source="mcp"):
        timeout = _timeout(timeout)
        self._capture_metadata = None
        if layer == "all":
            retained = self._retained.capture(eye, timeout, self._lane.wait)
            if retained is not None:
                data, self._capture_metadata = retained
                return data
        self._capture_metadata = {"transport": "legacy_files", "eye": eye, "layer": layer}
        self._artifacts.clear_screenshots(layer)
        requested_at = time.time()
        self._artifacts.publish(
            "screenshot_request.json",
            {
                "timestamp": requested_at,
                "eye": eye,
                "include_ui": True,
                "layer": layer,
                "requested_by": source,
            },
        )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self._artifacts.screenshot(requested_at, layer)
            if data:
                self._artifacts.clear_screenshots(layer)
                # The runtime owns acknowledgement/deletion of its request.
                # Never delete a newer externally replaced request marker.
                return data
            self._lane.wait(min(0.1, max(0.0, deadline - time.monotonic())))
        return None

    def screenshot(self, arguments):
        eye = arguments.get("eye", "both")
        with self._lane.admit():
            try:
                data = self._capture(
                    arguments.get("timeout", 5), eye, arguments.get("layer", "all")
                )
            except OSError as error:
                return [TextResult(f"Failed to request screenshot: {error}")]
            if data:
                try:
                    canonical = self._capture_metadata is not None and self._capture_metadata["transport"] == "retained_preview"
                    jpeg = to_jpeg(data, eye="both" if canonical else eye)
                except Exception as error:
                    return [
                        TextResult(
                            f"Captured {len(data)} bytes but failed to convert to JPEG: {error}. "
                            "Ensure Pillow is installed in the MCP environment."
                        )
                    ]
                return [
                    TextResult(
                        f"Screenshot captured ({len(data)} bytes raw -> {len(jpeg)} bytes JPEG "
                        f"@ q{SCREENSHOT_JPEG_QUALITY}, max width {SCREENSHOT_MAX_WIDTH}px, eye={eye})"
                        + ("\n" + json.dumps(self._capture_metadata) if self._capture_metadata is not None else "")
                    ),
                    ImageResult(jpeg),
                ]
            return [
                TextResult(
                    "Screenshot timeout. The OpenXR Simulator runtime may not be running, "
                    "or no application is actively rendering frames. Make sure:\n"
                    "1. An OpenXR application is running\n2. The OpenXR Simulator is set as the active runtime\n"
                    "3. The application is submitting frames"
                )
            ]

    def validate(self, arguments):
        with self._lane.admit():
            data = self._capture(arguments.get("timeout", 5))
            if not data:
                return [
                    TextResult(
                        "Screenshot timeout — can't validate stereo without a current frame."
                    )
                ]
            return [TextResult(json.dumps(validate_stereo_image(data), indent=2))]

    def consistency(self, arguments):
        try:
            import stereo_consistency as analysis
        except ImportError as error:
            return [TextResult(f"stereo_consistency.py could not be imported: {error}")]
        frames = max(1, min(30, int(arguments.get("frames", 1))))
        interval = _timeout(arguments.get("interval", 1))
        timeout = _timeout(arguments.get("timeout", 5))
        threshold = float(arguments.get("threshold", analysis.RESIDUAL_THRESHOLD))
        if not math.isfinite(threshold):
            raise ValueError("A finite stereo threshold is required")
        with self._lane.admit():
            frusta = self._frusta(analysis)
            worst, worst_sheet, lines = None, None, []
            for index in range(frames):
                started = time.monotonic()
                data = self._capture(timeout, source="stereo_consistency")
                if data is None:
                    lines.append(f"[{index}] screenshot timeout")
                    continue
                report, sheet = analysis.analyze_bytes(
                    data, threshold=threshold, frusta=frusta
                )
                lines.append(f"[{index}] {analysis.describe(report)}")
                if worst is None or report.score > worst.score:
                    worst, worst_sheet = report, sheet
                if index + 1 < frames:
                    self._lane.wait(max(0.0, interval - (time.monotonic() - started)))
            if worst is None:
                return [
                    TextResult(
                        "No screenshot arrived - is the app submitting projection layers?"
                    )
                ]
            result = [TextResult("\n".join(lines))]
            if arguments.get("include_image", True) and worst_sheet is not None:
                output = BytesIO()
                worst_sheet.convert("RGB").save(
                    output, format="JPEG", quality=80, optimize=True
                )
                result.append(ImageResult(output.getvalue()))
            return result

    def _frusta(self, analysis):
        try:
            document = self._projections.read(timeout=2)
            entry = document["entries"][-1]
            settings = self._artifacts.read_json(Artifact.SETTINGS, {})
            ipd = float(settings.get("ipd_mm", 64)) / 1000
            return analysis.frusta_from_fov(entry["left_fov"], entry["right_fov"], ipd)
        except (WorkflowBusy, OSError, ValueError, KeyError, IndexError, TypeError):
            return None


class IncidentWorkflow:
    """Own one preview/UI incident request until its new directory is observed."""

    def __init__(self, artifacts, *, ui=False):
        self._artifacts = artifacts
        self._ui = ui
        self._status = Artifact.UI_FLICKER if ui else Artifact.FLICKER
        self._request = (
            "ui_flicker_capture_request.json" if ui else "flicker_capture_request.json"
        )
        self._lane = RequestLane()

    def close(self):
        self._lane.close()

    def status(self, arguments):
        document = self._artifacts.read_json(self._status)
        if document is None:
            if not self._artifacts.exists(self._status):
                detail = (
                    "Start an OpenXR application that submits a D3D12 quad layer."
                    if self._ui
                    else "Start an OpenXR application with the instrumented simulator runtime."
                )
                return [TextResult(f"No {self._status.value} exists. {detail}")]
            name = "UI flicker" if self._ui else "flicker"
            return [
                TextResult(f"Failed to read {name} status: Invalid or unreadable JSON")
            ]
        age = max(0.0, time.time() - self._artifacts.modified(self._status))
        document["statusAgeSeconds"] = round(age, 3)
        document["statusFresh"] = age < 5
        result = [TextResult(json.dumps(document, indent=2))]
        if arguments.get("include_images", True) and document.get(
            "lastIncidentDirectory"
        ):
            try:
                directory = self._artifacts.incident_directory(
                    document["lastIncidentDirectory"]
                )
                sheet = build_flicker_contact_sheet(
                    directory, int(arguments.get("max_frames", 8))
                )
            except (ValueError, OSError, AttributeError):
                sheet = None
            if sheet:
                result.append(ImageResult(sheet))
        return result

    def capture(self, arguments):
        timeout = max(1.0, min(_timeout(arguments.get("timeout", 5)), 15))
        frames = max(2, min(int(arguments.get("max_frames", 12)), 12))
        with self._lane.admit():
            previous = self._artifacts.read_json(self._status, {}).get(
                "lastIncidentDirectory", ""
            )
            self._artifacts.publish(
                self._request,
                {
                    "requestedUnixMs": int(time.time() * 1000),
                    "source": "mcp-ui-only" if self._ui else "mcp",
                },
            )
            deadline = time.monotonic() + timeout
            directory, document = None, {}
            while time.monotonic() < deadline:
                document = self._artifacts.read_json(self._status, {})
                current = document.get("lastIncidentDirectory", "")
                if current and current != previous:
                    try:
                        candidate = self._artifacts.incident_directory(current)
                        if self._artifacts.preview_count(candidate) >= 2:
                            directory = candidate
                            break
                    except (OSError, ValueError):
                        pass
                self._lane.wait(0.1)
            if directory is None:
                detail = (
                    "Timed out waiting for a UI-only burst. No valid quad rectangle may be visible."
                    if self._ui
                    else "Timed out waiting for a manual composed-preview burst. The simulator may not be rendering new D3D12 preview frames."
                )
                return [TextResult(detail)]
            settle = min(deadline, time.monotonic() + 1)
            while (
                time.monotonic() < settle
                and self._artifacts.preview_count(directory) < frames
            ):
                self._lane.wait(0.1)
            sheet = build_flicker_contact_sheet(directory, frames)
            result = [
                TextResult(
                    json.dumps(
                        {
                            "captureSource": (
                                "openxr-simulator-ui-quad"
                                if self._ui
                                else "openxr-simulator-composed-preview"
                            ),
                            "incidentDirectory": str(directory),
                            "framesAvailable": self._artifacts.preview_count(directory),
                            "status": document,
                        },
                        indent=2,
                    )
                )
            ]
            if sheet:
                result.append(ImageResult(sheet))
            return result
