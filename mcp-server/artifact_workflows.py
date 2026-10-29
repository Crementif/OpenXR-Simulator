"""Owned capture/metadata workflows, interruptible waits, and CPU analysis."""

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
from retained_incident import RetainedIncident
from legacy_control import SessionBinding
from debug_control import _json_result
from tool_results import ImageResult, TextResult


class WorkflowBusy(RuntimeError):
    pass


class WorkflowClosed(RuntimeError):
    pass


class RequestLane:
    """Reject overlapping requests to one capture owner."""

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


def captured_frusta(metadata, from_fov):
    metadata = metadata or {}
    composition = metadata.get("composition")
    if composition is None or not metadata.get("frame") or composition.get("frame") != metadata["frame"]:
        return None
    try:
        fovs = [{"aL": eye["left"], "aR": eye["right"]} for eye in composition["fovs"]]
        return from_fov(fovs[0], fovs[1], float(composition["ipd"]))
    except (ValueError, KeyError, IndexError, TypeError):
        return None


class ProjectionWorkflow:
    def __init__(self, artifacts):
        self._lane = RequestLane()
        self._binding = SessionBinding(artifacts.directory, require_commands=False)

    def close(self):
        self._lane.close()

    def disconnect(self):
        self._binding.close()

    def read(self, count=64):
        with self._lane.admit():
            try:
                if not self._binding.connect():
                    raise RuntimeError('No runtime control endpoint found')
                from simxr_control import Capability
                client, session = self._binding.client, self._binding.session
                self._binding.validate_session()
                if not client.status(session)['capabilities'] & Capability.PROJECTION_HISTORY:
                    raise RuntimeError('The selected runtime does not support projection history')
                document = client.projection_history(session, count=count)['document']
                entries = []
                for record in document['entries']:
                    views = record['projection']['views']
                    entry = {'frame': str(record['frame_id']), 'displayTime': str(record['display_time']),
                             'layer': record['layer'], 'ipd': record['ipd'], 'view_count': len(views),
                             'projection': _json_result(record['projection'])}
                    if views:
                        pose = views[0]['pose_in_space']
                        entry['pose'] = dict(pose['position'], **{'q' + key: value for key, value in pose['orientation'].items()})
                    for name, view in zip(('left', 'right'), views):
                        fov, rectangle = view['fov'], view['color']['rectangle']
                        entry[name + '_fov'] = {short: fov[full] for short, full in
                                               (('aL', 'left'), ('aR', 'right'), ('aU', 'up'), ('aD', 'down'))}
                        entry[name + '_rect'] = [rectangle[key] for key in ('x', 'y', 'width', 'height')]
                    entries.append(entry)
                metadata = _json_result({key: value for key, value in document.items() if key != 'entries'})
                return dict(metadata, count=len(entries), entries=entries, transport='frame_history', session=str(session.id))
            except BaseException as error:
                if self._binding.is_uncertain(error):
                    self._binding.invalidate(error)
                raise

    def tool(self, arguments):
        limit = int(arguments.get("max_entries", 20))
        if not 1 <= limit <= 64:
            raise ValueError("Projection log max_entries must be in [1,64]")
        return [TextResult(json.dumps(self.read(count=limit), indent=2))]


class CaptureWorkflow:
    """Own screenshot capture and analysis for the complete admitted request.

    Multi-frame analysis retains ownership across its captures.
    Close interrupts waits without resubmitting a request.
    """

    def __init__(self, artifacts):
        self._lane = RequestLane()
        self._retained = RetainedScreenshot(artifacts.directory)
        self._capture_metadata = None

    def close(self):
        self._lane.close()

    def disconnect(self):
        # The application owner calls this after admitted workers have joined.
        self._retained.close()

    def capture(self, timeout=5, *, eye="both", layer="projection"):
        """Return pixels and their metadata under one complete request admission."""
        with self._lane.admit():
            data = self._capture(timeout, eye, layer)
            return data, self._capture_metadata

    def _capture(self, timeout, eye="both", layer="projection"):
        timeout = _timeout(timeout)
        self._capture_metadata = None
        data, self._capture_metadata = self._retained.capture(eye, timeout, self._lane.wait, layer)
        return data

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
                    jpeg = to_jpeg(data, eye="both")
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
        if not math.isfinite(threshold) or threshold <= 0:
            raise ValueError("A finite positive stereo threshold is required")
        with self._lane.admit():
            worst, worst_sheet, lines = None, None, []
            for index in range(frames):
                started = time.monotonic()
                data = self._capture(timeout)
                if data is None:
                    lines.append(f"[{index}] screenshot timeout")
                    continue
                frusta = captured_frusta(self._capture_metadata, analysis.frusta_from_fov)
                report, sheet = analysis.analyze_bytes(
                    data, threshold=threshold, frusta=frusta
                )
                geometry = "captured composition" if frusta is not None else "unavailable"
                frame = (self._capture_metadata or {}).get("frame", {}).get("frame_id", "unknown")
                lines.append(f"[{index}] frame={frame}, geometry={geometry}: {analysis.describe(report)}")
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


class IncidentWorkflow:
    """Own a native preview/UI incident capture and its saved artifacts."""

    def __init__(self, artifacts, *, ui=False):
        self._artifacts = artifacts
        self._ui = ui
        self._retained = RetainedIncident(artifacts.directory, ui=ui)
        self._status = Artifact.UI_FLICKER if ui else Artifact.FLICKER
        self._lane = RequestLane()

    def close(self):
        self._lane.close()

    def disconnect(self):
        if self._retained is not None:
            self._retained.close()

    def _retained_result(self, document, frames, include_images=True):
        result = [TextResult(json.dumps(document, indent=2))]
        if include_images:
            sheet = build_flicker_contact_sheet(self._artifacts.incident_directory(document['incidentDirectory']),
                                                frames, entire_window=True)
            if sheet:
                result.append(ImageResult(sheet))
        return result

    def status(self, arguments):
        if self._retained is not None and self._retained.latest is not None:
            return self._retained_result(self._retained.latest, int(arguments.get('max_frames', 8)),
                                         arguments.get('include_images', True))
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
            document = self._retained.capture(timeout, self._lane.wait)
            return self._retained_result(document, frames)
