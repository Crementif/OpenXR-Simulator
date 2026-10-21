"""Lifecycle and domain routing for the MCP server, independent of MCP transport."""

import asyncio
from concurrent.futures import ThreadPoolExecutor
import json
import os
import threading
from artifact_workflows import (
    CaptureWorkflow,
    IncidentWorkflow,
    ProjectionWorkflow,
    WorkflowBusy,
    WorkflowClosed,
)
from debug_control import DebugTools, TOOL_DEFINITIONS
from file_commands import FileEdit, encode_file_edit
from legacy_control import LegacyControl
from runtime_artifacts import CaptureFailed, RuntimeArtifacts, simulator_directory
from runtime_diagnostics import RuntimeDiagnostics
from tool_results import TextResult


class McpApplication:
    """Own clients, artifacts and admitted work until explicit joined shutdown.

    The executor may run different tool domains concurrently. Only short
    admission bookkeeping uses the application mutex; transport/file/Pillow
    work never holds it. Artifact lanes reject conflicting capture workflows.
    """

    MAX_ACTIVE = 16
    WORKERS = 4

    def __init__(self, directory=None, *, debug=None, legacy=None, platform=None):
        self._platform = os.name if platform is None else platform
        self._artifacts = RuntimeArtifacts(
            simulator_directory() if directory is None else directory,
            platform=self._platform,
        )
        self._debug = DebugTools() if debug is None else debug
        self._legacy = (
            LegacyControl(self._artifacts.directory) if legacy is None else legacy
        )
        self._control_tools = frozenset(
            definition["name"] for definition in TOOL_DEFINITIONS
        )
        self._diagnostics = RuntimeDiagnostics(self._artifacts)
        self._projection = ProjectionWorkflow(self._artifacts)
        self._capture = CaptureWorkflow(self._artifacts, self._projection)
        self._preview_incidents = IncidentWorkflow(self._artifacts)
        self._ui_incidents = IncidentWorkflow(self._artifacts, ui=True)
        self._handlers = {
            "capture_screenshot": self._capture.screenshot,
            "validate_stereo": self._capture.validate,
            "analyze_stereo_consistency": self._capture.consistency,
            "get_projection_log": self._projection.tool,
            "get_frame_info": self._diagnostics.frame,
            "get_diagnostics": self._diagnostics.summary,
            "diagnose_issue": self._diagnostics.issue,
            "read_logs": self._diagnostics.logs,
            "clear_logs": self._diagnostics.clear,
            "get_session_state": self._diagnostics.session,
            "get_head_tracking": self._diagnostics.head,
            "get_flicker_status": self._preview_incidents.status,
            "capture_flicker_window": self._preview_incidents.capture,
            "get_ui_flicker_status": self._ui_incidents.status,
            "capture_ui_flicker_window": self._ui_incidents.capture,
        }
        self._changed = threading.Condition()
        self._lifecycle = threading.Lock()
        self._active = 0
        self._closed = False
        self._stopped = False
        self._executor = ThreadPoolExecutor(
            max_workers=self.WORKERS, thread_name_prefix="simxr-mcp"
        )
        self._futures = set()

    async def call_async(self, name, arguments):
        # Admission precedes executor submission, so cancelled/queued MCP calls
        # count against the same bound as running work. wrap_future propagates
        # cancellation to not-yet-started jobs; running jobs retain admission and
        # artifact lanes until their actual concurrent Future settles.
        with self._changed:
            rejected = self._admission_error()
            if rejected is not None:
                return rejected
            self._active += 1
            try:
                future = self._executor.submit(self._dispatch, name, arguments)
            except BaseException:
                self._active -= 1
                self._changed.notify_all()
                raise
            self._futures.add(future)
        future.add_done_callback(self._settled)
        return await asyncio.wrap_future(future)

    def _admission_error(self):
        if self._closed:
            return [TextResult("MCP application closed")]
        if self._active >= self.MAX_ACTIVE:
            return [
                TextResult("MCP application request capacity reached; retry explicitly")
            ]
        return None

    def _settled(self, future):
        with self._changed:
            self._futures.remove(future)
            self._active -= 1
            self._changed.notify_all()

    def call(self, name, arguments):
        # Synchronous callers share admission but do not occupy executor jobs.
        with self._changed:
            rejected = self._admission_error()
            if rejected is not None:
                return rejected
            self._active += 1
        try:
            return self._dispatch(name, arguments)
        finally:
            with self._changed:
                self._active -= 1
                self._changed.notify_all()

    def _dispatch(self, name, arguments):
        try:
            # Modern operation errors remain modern results, including unsupported
            # ImageExport/GpuPreview. There is never an artifact fallback here.
            if name in self._control_tools:
                return [
                    TextResult(json.dumps(self._debug.call(name, arguments), indent=2))
                ]
            result = self._legacy.call(name, arguments)
            if result is not None:
                return [TextResult(json.dumps(result, indent=2))]
            handler = self._handlers.get(name)
            if handler is not None:
                return handler(arguments)
            edit = encode_file_edit(name, arguments, self._platform)
            if isinstance(edit, FileEdit):
                self._artifacts.publish_bytes(edit.filename, edit.document)
                return [TextResult(edit.reply)]
            if isinstance(edit, TextResult):
                return [edit]
            return [TextResult(f"Unknown tool: {name}")]
        except (WorkflowBusy, WorkflowClosed, CaptureFailed) as error:
            return [TextResult(str(error))]

    def close(self):
        with self._lifecycle:
            if self._stopped:
                return
            with self._changed:
                self._closed = True
            self._capture.close()
            self._projection.close()
            self._preview_incidents.close()
            self._ui_incidents.close()
            # Cancel queued work and join admitted workers. A cancelled async
            # waiter cannot outlive this owner or release another job's lane.
            self._executor.shutdown(wait=True, cancel_futures=True)
            with self._changed:
                self._changed.wait_for(lambda: self._active == 0)
            # SDK connection/source retirement happens after all callers settle,
            # without holding application state or artifact locks.
            try:
                try:
                    self._capture.disconnect()
                finally:
                    self._legacy.close()
            finally:
                try:
                    self._debug.close()
                finally:
                    self._artifacts.close()
                    self._stopped = True
