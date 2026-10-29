"""MCP owner/routing/concurrency tests. CPU files are compatibility fixtures."""

import asyncio
import importlib.util
from concurrent.futures import ThreadPoolExecutor
from io import BytesIO, StringIO
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "mcp-server"))
sys.path.insert(0, str(ROOT / "sdk/python"))
from mcp_application import McpApplication
from tool_catalog import tool_definitions
from tool_results import ImageResult, TextResult

HOST = None


def wait_for(predicate, timeout=3):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("CPU fixture did not reach expected state")
        time.sleep(0.005)


class DebugFixture:
    def __init__(self):
        self.calls = []
        self.closed = 0
        self.result = {
            "ok": False,
            "error": "unsupported_command",
            "operation_id": str(2**64 - 1),
        }

    def call(self, name, arguments):
        self.calls.append((name, arguments))
        return self.result

    def close(self):
        self.closed += 1


class LegacyFixture:
    def __init__(self):
        self.calls = []
        self.closed = 0
        self.result = None

    def call(self, name, arguments):
        self.calls.append((name, arguments))
        return self.result

    def close(self):
        self.closed += 1


class ApplicationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="simxr-mcp-owners-")
        self.root = Path(self.directory.name)
        self.debug = DebugFixture()
        self.legacy = LegacyFixture()
        self.app = McpApplication(
            self.root, debug=self.debug, legacy=self.legacy
        )
        self.addCleanup(self.directory.cleanup)
        self.addCleanup(self.app.close)

    def test_stereo_analysis_uses_only_matching_captured_composition(self):
        frusta = object()
        analysis = SimpleNamespace(RESIDUAL_THRESHOLD=.1, frusta_from_fov=Mock(return_value=frusta),
                                   analyze_bytes=Mock(return_value=(SimpleNamespace(score=1), None)),
                                   describe=lambda report: "checked")
        fov = {"left": -.5, "right": .7}
        first = str(2**53 + 1)
        metadata = {"frame": {"generation": "1", "frame_id": first},
                    "composition": {"frame": {"generation": "1", "frame_id": first},
                                    "fovs": [fov, fov], "ipd": .07}}
        def capture(*args, **kwargs):
            self.app._capture._capture_metadata = metadata
            if analysis.analyze_bytes.call_count:
                metadata["frame"] = {"generation": "1", "frame_id": str(2**53 + 2)}
            return b"captured image"
        with patch.dict(sys.modules, {"stereo_consistency": analysis}), \
             patch.object(self.app._capture, "_capture", side_effect=capture), \
             patch.object(self.app._projection, "read", side_effect=AssertionError("Live geometry substituted")):
            result = self.app.call("analyze_stereo_consistency", {"frames": 2, "interval": 0, "include_image": False})
        analysis.frusta_from_fov.assert_called_once_with({"aL": -.5, "aR": .7}, {"aL": -.5, "aR": .7}, .07)
        self.assertIs(analysis.analyze_bytes.call_args_list[0].kwargs["frusta"], frusta)
        self.assertIsNone(analysis.analyze_bytes.call_args_list[1].kwargs["frusta"])
        self.assertIn("frame=" + first + ", geometry=captured composition", result[0].text)
        self.assertIn("geometry=unavailable", result[0].text)

    @unittest.skipUnless(importlib.util.find_spec("PIL") and importlib.util.find_spec("numpy"), "Pillow and NumPy are required")
    def test_stereo_watch_uses_retained_frames_and_reports_missing_samples(self):
        import stereo_consistency as analysis

        fov = {"left": -.5, "right": .7}
        frame = {"generation": "1", "frame_id": str(2**53 + 1)}
        composition = {"frame": frame, "fovs": [fov, fov], "ipd": .07}
        metadata = {"transport": "retained_capture", "frame": frame, "composition": composition}
        mismatched = {**metadata, "frame": {**frame, "frame_id": str(2**53 + 2)}}
        report = analysis.Report("PASS", 0, 0, 100, 0, 0, 0, 0, 0)
        output = self.root / "stéréo 眼"
        with patch("artifact_workflows.RetainedScreenshot") as retained, \
             patch("artifact_workflows.ProjectionWorkflow.read", side_effect=AssertionError("Live geometry substituted")), \
             patch.object(analysis, "analyze_bytes", return_value=(report, None)) as analyze, \
             patch("sys.stdout", new_callable=StringIO):
            retained.return_value.capture.side_effect = [
                (b"first", metadata), (b"second", mismatched), TimeoutError("No completed capture")]
            results = analysis.watch(3, 0, output, self.root)
            retained.return_value.close.assert_called_once_with()
        self.assertIsNotNone(analyze.call_args_list[0].kwargs["frusta"])
        self.assertIsNone(analyze.call_args_list[1].kwargs["frusta"])
        self.assertEqual(results[0]["capture"]["frame"], frame)
        self.assertEqual(results[2]["verdict"], "CAPTURE_FAILED")
        self.assertIn("No completed capture", results[2]["error"])
        saved = [json.loads(line) for line in (output / "stereo_watch.jsonl").read_text(encoding="utf-8").splitlines()]
        self.assertEqual(saved, results)
        self.assertFalse((self.root / "screenshot_request.json").exists())
        self.assertFalse((self.root / "projection_log_dump_request").exists())
        with patch.object(analysis, "watch", return_value=results), patch("sys.stdout", new_callable=StringIO):
            self.assertEqual(analysis.main(["--watch", "3"]), 1)

    @unittest.skipUnless(importlib.util.find_spec("PIL") and importlib.util.find_spec("numpy"), "Pillow and NumPy are required")
    def test_stereo_watch_closes_capture_owner_after_output_failure(self):
        import stereo_consistency as analysis

        output = self.root / "watch"
        (output / "stereo_watch.jsonl").mkdir(parents=True)
        with patch("artifact_workflows.RetainedScreenshot") as retained, patch("sys.stdout", new_callable=StringIO):
            retained.return_value.capture.return_value = (None, {})
            with self.assertRaises(OSError):
                analysis.watch(1, 0, output, self.root)
            retained.return_value.close.assert_called_once_with()
        for arguments in (["--watch", "-1"], ["--watch", "1", "--interval", "nan"],
                          ["--watch", "1", "--threshold", "0"]):
            with patch.object(analysis, "watch") as watch, patch("sys.stderr", new_callable=StringIO):
                with self.assertRaises(SystemExit) as rejected:
                    analysis.main(arguments)
                self.assertEqual(rejected.exception.code, 2)
                watch.assert_not_called()

    @unittest.skipUnless(importlib.util.find_spec("PIL") and importlib.util.find_spec("numpy"), "Pillow and NumPy are required")
    def test_stereo_analysis_bounds_portrait_work_and_normalizes_eye_modes(self):
        from PIL import Image
        import stereo_consistency as analysis

        with Image.new("L", (80, 320), 64) as left, Image.new("RGBA", (80, 320), (64, 64, 64, 255)) as right:
            with patch.object(analysis, "_prepare", wraps=analysis._prepare) as prepare:
                report, _ = analysis.analyze_pair(left, right)
                self.assertEqual(report.verdict, "INCONCLUSIVE_NO_FEATURES")
                self.assertEqual([call.args[1] for call in prepare.call_args_list], [120, 120])
            for threshold in (0, -1, float("nan"), float("inf")):
                with self.assertRaisesRegex(ValueError, "positive stereo threshold"):
                    analysis.analyze_pair(left, right, threshold=threshold)
        with Image.new("RGB", (1, 10000)) as narrow:
            with patch.object(analysis, "_prepare", side_effect=AssertionError("Unbounded image allocation")):
                with self.assertRaisesRegex(ValueError, "too narrow"):
                    analysis.analyze_pair(narrow, narrow)
        self.assertIsNone(analysis.frusta_from_fov({"aL": float("nan"), "aR": .5}, {"aL": -.5, "aR": .5}))

    def test_modern_errors_and_full_u64_never_fall_back_to_files(self):
        result = self.app.call("debug_start_export", {"image": str(2**64 - 1)})
        self.assertEqual(json.loads(result[0].text), self.debug.result)
        self.assertEqual(self.legacy.calls, [])
        self.assertEqual(list(self.root.iterdir()), [])

    def test_acknowledged_compatibility_error_never_becomes_file_edit(self):
        self.legacy.result = {
            "ok": False,
            "transport": "simulation_commands",
            "phase": "queued",
            "transaction_id": "17",
        }
        result = self.app.call("set_head_pose", {"x": 4})
        self.assertEqual(json.loads(result[0].text), self.legacy.result)
        self.assertFalse((self.root / "head_pose_command.json").exists())

    def test_compatibility_reads_state_after_prior_transaction_settles(self):
        from legacy_control import LegacyControl
        from simxr_control import Session

        tools = LegacyControl(self.root)
        binding = Mock()
        binding.connect.return_value = True
        binding.session = Session(1, 1)
        tools._binding = binding
        tools._sequence = Mock()
        tools._admission = Mock()
        state = {'frame_id': 9, 'configuration': {'head': {
            'position': [0, 0, 0], 'orientation': [0, 0, 0, 1]}}}

        def settle(_binding):
            state['frame_id'] = 10
            state['configuration']['head']['orientation'] = [0, 2**-.5, 0, 2**-.5]
            return None

        binding.observe.side_effect = lambda: json.loads(json.dumps(state))
        binding.client.schedule.return_value = {'phase': 'queued', 'transaction_id': 2}
        tools._admission.settle.side_effect = settle
        tools._admission.accepted.return_value = {'ok': True}
        try:
            result = tools.call('set_controller_pose', {'hand': 0, 'posX': 0, 'posY': 0, 'posZ': -1})
            self.assertTrue(result['ok'])
            scheduled = binding.client.schedule.call_args
            command = scheduled.args[1][1]
            self.assertAlmostEqual(command['position'][0], -1, places=6)
            self.assertAlmostEqual(command['position'][2], 0, places=6)
            self.assertEqual(scheduled.kwargs['frame'], 11)

            binding.observe.reset_mock()
            tools._admission.settle.side_effect = None
            tools._admission.settle.return_value = {'ok': False, 'phase': 'queued'}
            result = tools.call('set_controller_pose', {'hand': 0})
            self.assertFalse(result['ok'])
            binding.observe.assert_not_called()
        finally:
            tools.close()

    def test_projection_requires_native_history_and_preserves_existing_files(self):
        from simxr_control import Capability, Session

        saved = self.root / "projection_log.json"
        saved.write_text('{"entries": []}')
        with patch("simxr_control.discover", return_value=[]):
            with self.assertRaisesRegex(RuntimeError, "No runtime control endpoint"):
                self.app.call("get_projection_log", {})
        session = Session(1, 1)
        endpoint = Mock()
        endpoint.sessions.return_value = [session]
        endpoint.status.return_value = {"capabilities": Capability(0)}
        with patch("simxr_control.discover", return_value=["chosen.sock"]), \
             patch("simxr_control.Client", return_value=endpoint):
            with self.assertRaisesRegex(RuntimeError, "does not support projection history"):
                self.app.call("get_projection_log", {})
            endpoint.status.return_value = {"capabilities": Capability.PROJECTION_HISTORY}
            endpoint.projection_history.return_value = {"document": {"entries": [], "count": 0}}
            result = json.loads(self.app.call("get_projection_log", {"max_entries": 2})[0].text)
            self.assertEqual(result["entries"], [])
            self.assertEqual(result["transport"], "frame_history")
            endpoint.projection_history.assert_called_once_with(session, count=2)
        self.assertEqual(saved.read_text(), '{"entries": []}')
        self.assertEqual(list(self.root.iterdir()), [saved])

    def test_controls_without_endpoint_fail_without_writing_commands(self):
        from legacy_control import LegacyControl, TOOLS
        from pose_sequence import TOOLS as SEQUENCE_TOOLS

        app = McpApplication(self.root, debug=self.debug, legacy=LegacyControl(self.root))
        self.addCleanup(app.close)
        with patch("simxr_control.discover", return_value=[]):
            for name in TOOLS | SEQUENCE_TOOLS:
                with self.subTest(name=name):
                    result = json.loads(app.call(name, {})[0].text)
                    self.assertFalse(result["ok"])
                    self.assertIn("No runtime control endpoint", result["error"])
        self.assertEqual(list(self.root.iterdir()), [])

    def block_capture(self):
        entered = threading.Event()

        def capture(eye, timeout, wait, layer):
            entered.set()
            wait(30)
            raise AssertionError("Capture wait was not interrupted")

        active = patch.object(self.app._capture._retained, "capture", side_effect=capture)
        active.start()
        self.addCleanup(active.stop)
        return entered

    def test_conflicting_capture_and_close_interrupts_wait(self):
        entered = self.block_capture()
        with ThreadPoolExecutor(max_workers=2) as workers:
            pending = workers.submit(
                self.app.call, "capture_screenshot", {"timeout": 30}
            )
            wait_for(entered.is_set)
            conflict = self.app.call("validate_stereo", {"timeout": 30})
            self.assertIn("already owns", conflict[0].text)
            # Another independent domain remains available while capture waits.
            self.assertIn("session_state", self.app.call("get_frame_info", {})[0].text)
            started = time.monotonic()
            self.app.close()
            self.assertLess(time.monotonic() - started, 1)
            self.assertIn("closed", pending.result(timeout=1)[0].text)
        self.app.close()
        self.assertEqual((self.debug.closed, self.legacy.closed), (1, 1))

    def test_async_event_loop_and_status_remain_responsive_during_capture(self):
        entered = self.block_capture()
        async def scenario():
            pending = asyncio.create_task(
                self.app.call_async("capture_screenshot", {"timeout": 30})
            )
            for _ in range(200):
                if entered.is_set():
                    break
                await asyncio.sleep(0.005)
            else:
                self.fail("Async capture was never admitted")
            status = await asyncio.wait_for(
                self.app.call_async("get_frame_info", {}), 0.5
            )
            self.assertIsInstance(status[0], TextResult)
            self.assertFalse(pending.done())
            await asyncio.to_thread(self.app.close)
            self.assertIn("closed", (await asyncio.wait_for(pending, 1))[0].text)

        asyncio.run(scenario())

    def test_cancelled_async_waiter_does_not_release_capture_worker_ownership(self):
        entered = self.block_capture()
        async def scenario():
            pending = asyncio.create_task(
                self.app.call_async("capture_screenshot", {"timeout": 30})
            )
            for _ in range(200):
                if entered.is_set():
                    break
                await asyncio.sleep(0.005)
            else:
                self.fail("Async capture was never admitted")
            pending.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await pending
            conflict = await self.app.call_async("capture_screenshot", {"timeout": 1})
            self.assertIn("already owns", conflict[0].text)
            # The worker, rather than the cancelled coroutine, retains the lane
            # and admission. Joined close interrupts its wait and drains it.
            await asyncio.wait_for(asyncio.to_thread(self.app.close), 1)
            self.assertIn("closed", self.app.call("capture_screenshot", {})[0].text)

        asyncio.run(scenario())

    def test_queued_async_admission_is_bounded_cancelled_and_joined_on_close(self):
        class BlockingDebug(DebugFixture):
            def __init__(self):
                super().__init__()
                self.entered = threading.Event()
                self.release = threading.Event()

            def call(self, name, arguments):
                self.calls.append((name, arguments))
                self.entered.set()
                if not self.release.wait(3):
                    raise AssertionError("Test did not release admitted worker")
                return self.result

        async def scenario():
            debug = BlockingDebug()
            with patch.object(McpApplication, "WORKERS", 1):
                app = McpApplication(self.root, debug=debug, legacy=LegacyFixture())
            tasks = []
            try:
                tasks.append(asyncio.create_task(app.call_async("debug_status", {})))
                while not debug.entered.is_set():
                    await asyncio.sleep(0.005)
                tasks.extend(
                    asyncio.create_task(app.call_async("debug_status", {}))
                    for _ in range(15)
                )
                await asyncio.sleep(0.02)
                rejected = await app.call_async("debug_status", {})
                self.assertIn("capacity", rejected[0].text)
                tasks[1].cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await tasks[1]
                # Cancelling an unstarted job recovers one admission. The new
                # call queues, proving it was not silently rejected or run.
                tasks.append(asyncio.create_task(app.call_async("debug_status", {})))
                await asyncio.sleep(0.02)
                self.assertFalse(tasks[-1].done())
                closing = asyncio.create_task(asyncio.to_thread(app.close))
                for _ in range(200):
                    if "closed" in app.call("unknown", {})[0].text:
                        break
                    await asyncio.sleep(0.005)
                else:
                    self.fail("Close did not revoke admission")
                self.assertFalse(closing.done())
                debug.release.set()
                await asyncio.wait_for(closing, 1)
                results = await asyncio.gather(*tasks, return_exceptions=True)
                self.assertIsInstance(results[0][0], TextResult)
                self.assertTrue(
                    all(
                        isinstance(value, asyncio.CancelledError)
                        for value in results[1:]
                    )
                )
                self.assertEqual(len(debug.calls), 1)
                self.assertEqual(debug.closed, 1)
            finally:
                debug.release.set()
                await asyncio.to_thread(app.close)
                await asyncio.gather(*tasks, return_exceptions=True)

        asyncio.run(scenario())

    def test_event_loop_shutdown_does_not_orphan_running_admission(self):
        entered = threading.Event()
        release = threading.Event()

        def blocked(name, arguments):
            entered.set()
            if not release.wait(3):
                raise AssertionError("Test did not release worker after loop shutdown")
            return self.debug.result

        async def start_only():
            asyncio.create_task(self.app.call_async("debug_status", {}))
            while not entered.is_set():
                await asyncio.sleep(0.005)
            # asyncio.run cancels this task, while its already-running worker
            # remains owned by McpApplication rather than the closed loop.

        with patch.object(self.app, "MAX_ACTIVE", 1), patch.object(
            self.debug, "call", blocked
        ):
            try:
                asyncio.run(start_only())
                self.assertIn("capacity", self.app.call("get_frame_info", {})[0].text)
                release.set()
                wait_for(
                    lambda: "capacity"
                    not in self.app.call("get_frame_info", {})[0].text
                )
                self.assertIsInstance(
                    self.app.call("get_frame_info", {})[0], TextResult
                )
            finally:
                release.set()

    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow is not installed")
    def test_stereo_analysis_decodes_actual_grayscale_image(self):
        from PIL import Image
        from image_analysis import validate_stereo_image

        image = Image.new("RGB", (64, 32))
        image.putdata(
            [(value % 256, value % 256, value % 256) for value in range(64 * 32)]
        )
        encoded = BytesIO()
        image.save(encoded, format="BMP")
        result = validate_stereo_image(encoded.getvalue())
        if result.get("error", "").startswith("numpy required"):
            self.skipTest("NumPy is not installed")
        self.assertIn("verdict", result)
        self.assertIn("side_by_side", result["all_layouts"])

    @unittest.skipUnless(importlib.util.find_spec("PIL") and importlib.util.find_spec("numpy"), "Pillow and NumPy are required")
    def test_extreme_image_shapes_have_bounded_finite_results(self):
        from PIL import Image
        from image_analysis import to_jpeg, validate_stereo_image

        with Image.new("RGB", (4096, 1)) as wide:
            encoded = BytesIO()
            wide.save(encoded, format="PNG")
        with Image.open(BytesIO(to_jpeg(encoded.getvalue()))) as jpeg:
            self.assertEqual(jpeg.size, (1280, 1))
        for size in ((1, 1), (2, 10000)):
            with Image.new("RGB", size) as image:
                encoded = BytesIO()
                image.save(encoded, format="PNG")
            result = validate_stereo_image(encoded.getvalue())
            self.assertIn("error", result)
            json.dumps(result, allow_nan=False)

    def test_capture_without_endpoint_does_not_publish_requests(self):
        with patch("simxr_control.discover", return_value=[]):
            for tool in ("capture_screenshot", "capture_flicker_window", "capture_ui_flicker_window"):
                with self.subTest(tool=tool):
                    result = self.app.call(tool, {})
                    self.assertIn("No runtime control endpoint", result[0].text)
        self.assertEqual(list(self.root.iterdir()), [])

    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow is not installed")
    def test_retained_eye_is_not_cropped_twice(self):
        from PIL import Image

        buffer = BytesIO()
        Image.new("RGB", (4, 2), (255, 0, 0)).save(buffer, format="BMP")
        with patch.object(self.app._capture._retained, "capture",
                          return_value=(buffer.getvalue(), {"transport": "retained_preview"})) as capture:
            result = self.app.call("capture_screenshot", {"eye": "left"})
        self.assertEqual(capture.call_args.args[0], "left")
        self.assertIsInstance(result[1], ImageResult)
        with Image.open(BytesIO(result[1].data)) as converted:
            self.assertEqual(converted.size, (4, 2))
        self.assertEqual(list(self.root.iterdir()), [])

    def test_retained_incident_publishes_only_after_window_release(self):
        from retained_incident import RetainedIncident
        from runtime_artifacts import CaptureFailed
        from simxr_control import Capability, Session

        for failed in (False, True):
            with self.subTest(failed=failed), tempfile.TemporaryDirectory() as temporary:
                session = Session(1, 1)
                client = Mock()
                client.sessions.return_value = [session]
                client.status.return_value = {'capabilities': Capability.IMAGE_WINDOW | Capability.IMAGE_EXPORT}
                client.begin_image_window.return_value = {'image_window_id': 9}
                client.image_window.return_value = {'document': {
                    'phase': 'complete', 'generation': 1, 'pre_count': 1, 'post_count': 12,
                    'entries': [{'frame_id': frame, 'phase': 'clear'} for frame in range(1, 14)]}}
                incident = RetainedIncident(temporary)
                incident._binding = Mock(client=client, session=session)
                incident._binding.connect.return_value = True
                incident._binding.is_uncertain.return_value = False
                previous = {'previous': True}
                incident.latest = previous

                def release(window):
                    self.assertEqual(window, 9)
                    self.assertIs(incident.latest, previous)
                    if failed:
                        raise RuntimeError('Window release failed')

                client.release_image_window.side_effect = release
                if failed:
                    with self.assertRaisesRegex(CaptureFailed, 'Window release failed'):
                        incident.capture(5, lambda seconds: None)
                    self.assertIs(incident.latest, previous)
                    self.assertFalse(list(Path(temporary).rglob('incident_*')))
                else:
                    result = incident.capture(5, lambda seconds: None)
                    self.assertIs(incident.latest, result)
                    self.assertTrue((Path(result['incidentDirectory']) / 'incident.json').is_file())
                    self.assertEqual(result['framesAvailable'], 13)
                client.release_image_window.assert_called_once_with(9)

    def test_status_and_incident_scope_never_reads_a_foreign_directory(self):
        (self.root / "flicker_status.json").write_text(
            json.dumps({"lastIncidentDirectory": str(self.root.parent)})
        )
        with patch(
            "artifact_workflows.build_flicker_contact_sheet",
            side_effect=AssertionError("foreign image access"),
        ):
            result = self.app.call("get_flicker_status", {"include_images": True})
        self.assertEqual(len(result), 1)
        self.assertIn("statusAgeSeconds", json.loads(result[0].text))

    def test_discovery_status_uses_actual_log_owner_and_preserves_stale_notes(self):
        log = self.root / f"openxr_simulator.{os.getpid()}.log"
        log.write_text(
            "[SimXR] PushState: Session 3 -> FOCUSED\n"
            "[SimXR] xrCreateSession: SUCCESS (Vulkan)\n"
            "[SimXR] xrEndFrame called (frame #7)\n"
        )
        (self.root / "runtime_status.json").write_text(
            json.dumps(
                {
                    "frame_count": 9,
                    "session_state": "VISIBLE",
                    "head_tracking": {
                        "position": {"x": 2, "y": 1.7, "z": 0},
                        "yaw": 3.141592653589793 / 2,
                    },
                }
            )
        )
        diagnostics = json.loads(self.app.call("get_diagnostics", {})[0].text)
        self.assertTrue(diagnostics["runtime_live"])
        self.assertEqual(diagnostics["graphics_api"], "Vulkan")
        self.assertEqual(diagnostics["frame_count"], 9)
        self.assertIn(
            "Session State: VISIBLE", self.app.call("get_session_state", {})[0].text
        )
        self.assertIn("yaw 90.0 deg", self.app.call("get_head_tracking", {})[0].text)

    def test_tool_schema_values_are_not_shared_across_calls(self):
        first = tool_definitions("posix")
        modern = next(item for item in first if item["name"] == "debug_step")
        modern["inputSchema"]["required"].append("must_not_leak")
        again = next(
            item for item in tool_definitions("posix") if item["name"] == "debug_step"
        )
        self.assertNotIn("must_not_leak", again["inputSchema"]["required"])
        self.assertNotIn("set_headset_profile", {item["name"] for item in first})
        self.assertNotIn(
            "set_headset_profile", {item["name"] for item in tool_definitions("nt")}
        )

@unittest.skipUnless(HOST, "C++ endpoint host was not supplied")
class EndpointTests(unittest.TestCase):
    def test_real_protocol_application_routes_full_u64_and_preserves_errors(self):
        with tempfile.TemporaryDirectory(prefix="simxr-mcp-app-endpoint-") as temporary:
            directory = Path(temporary)
            path, marker = directory / "control.sock", directory / "start"
            process = subprocess.Popen(
                [HOST, str(path), str(marker)],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            app = McpApplication(directory)
            try:
                wait_for(path.exists)

                def call(name, arguments):
                    return json.loads(app.call(name, arguments)[0].text)

                connected = call("debug_connect", {"path": str(path)})
                self.assertTrue(connected["ok"], connected)
                (session,) = connected["sessions"]
                common = {"connection": connected["connection"], **session}
                self.assertIsInstance(session["session"], str)
                lease = call("debug_acquire", common)["lease"]
                pause = call(
                    "debug_pause",
                    {**common, "lease": lease, "command_id": str(2**53 + 1)},
                )
                self.assertTrue(pause["ok"], pause)
                operation = pause["operation_id"]
                wait_for(
                    lambda: call("debug_operation", {**common, "operation": operation})[
                        "phase"
                    ]
                    == "complete"
                )
                status = call("debug_status", common)
                self.assertTrue(status["ok"], status)
                resumed = call(
                    "debug_resume",
                    {**common, "lease": lease, "command_id": str(2**53 + 2)},
                )
                self.assertTrue(resumed["ok"], resumed)
                app.close()
                marker.touch()
                output = process.communicate(timeout=5)[0]
                self.assertEqual(process.returncode, 0, output)
                self.assertFalse((directory / "head_pose_command.json").exists())
            finally:
                app.close()
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)


@unittest.skipUnless(importlib.util.find_spec("mcp"), "MCP SDK is not installed")
class StdioTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow is not installed")
    def test_real_mcp_stdio_control_and_unsupported_capture(self):
        if not HOST:
            self.skipTest("C++ endpoint host was not supplied")
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client
        from PIL import Image

        async def scenario():
            with tempfile.TemporaryDirectory(
                prefix="simxr-mcp-app-stdio-"
            ) as temporary:
                directory = Path(temporary)
                endpoint, marker = directory / "control.sock", directory / "start"
                process = subprocess.Popen(
                    [HOST, str(endpoint), str(marker)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                try:
                    await asyncio.to_thread(wait_for, endpoint.exists)
                    environment = dict(
                        os.environ,
                        SIMXR_DATA_DIR=temporary,
                        PYTHONPATH=os.pathsep.join(
                            filter(
                                None,
                                (
                                    str(ROOT / "sdk/python"),
                                    os.environ.get("PYTHONPATH"),
                                ),
                            )
                        ),
                    )
                    environment.pop("SIMXR_CONTROL_SOCKET", None)
                    options = StdioServerParameters(
                        command=sys.executable,
                        args=[str(ROOT / "mcp-server/openxr_simulator_mcp.py")],
                        env=environment,
                    )
                    async with stdio_client(options) as (reader, writer):
                        async with ClientSession(reader, writer) as client:
                            await client.initialize()
                            schemas = (await client.list_tools()).tools
                            self.assertEqual(
                                {tool.name for tool in schemas},
                                {tool["name"] for tool in tool_definitions()},
                            )

                            async def call(name, arguments):
                                response = await client.call_tool(name, arguments)
                                self.assertFalse(response.isError, response)
                                return json.loads(response.content[0].text)

                            connected = await call(
                                "debug_connect", {"path": str(endpoint)}
                            )
                            self.assertTrue(connected["ok"], connected)
                            (session,) = connected["sessions"]
                            common = {"connection": connected["connection"], **session}
                            self.assertIsInstance(session["session"], str)
                            self.assertIsInstance(session["generation"], str)
                            missing = await call(
                                "debug_start_export",
                                {
                                    "connection": connected["connection"],
                                    "image": str(2**64 - 1),
                                },
                            )
                            self.assertFalse(missing["ok"], missing)
                            self.assertEqual(missing["error"], "control_error")
                            self.assertEqual(
                                missing["code"], 3
                            )  # CPU host has no image provider.
                            self.assertFalse(
                                (directory / "screenshot_request.json").exists()
                            )
                            result = await asyncio.wait_for(
                                client.call_tool("capture_screenshot", {"timeout": 3}), 2)
                            self.assertIn("does not support screenshot capture", result.content[0].text)
                            self.assertFalse((directory / "screenshot_request.json").exists())
                            status = await asyncio.wait_for(call("debug_status", common), 0.5)
                            self.assertTrue(status["ok"], status)
                            await call(
                                "debug_disconnect",
                                {"connection": connected["connection"]},
                            )
                    marker.touch()
                    output = (await asyncio.to_thread(process.communicate, timeout=5))[
                        0
                    ]
                    self.assertEqual(process.returncode, 0, output)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=5)

        asyncio.run(scenario())


if __name__ == "__main__":
    if len(sys.argv) == 2 and not sys.argv[1].startswith("-"):
        HOST = sys.argv.pop(1)
        EndpointTests.__unittest_skip__ = False
    unittest.main()
