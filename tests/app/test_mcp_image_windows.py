#!/usr/bin/env python3
"""Exercise native MCP PNG/JPEG exports against a Windows Vulkan XR process."""
import base64
import http.client
from io import BytesIO
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlsplit
from PIL import Image

from mcp_support import Client, McpTest, eventually


def main():
    app, connector, probe, manifest = (str(Path(value).resolve()) for value in sys.argv[1:5])
    validate = os.environ.get("SIMXR_TEST_VALIDATE") == "1"
    with McpTest('simxr-mcp-image-', windowless=True) as test:
        root = test.root

        log = test.log('bridge.log')
        bridge = test.bridge(connector, "--app", app, "--windowless", log=log)
        try:
            client = Client(bridge)
            initialized = client.initialize('windows-image-test')
            assert "result" in initialized, initialized

            profiles = client.tool("get_headset_profiles", {})
            assert not profiles["error"], profiles
            index = next(item for item in profiles["data"]["profiles"] if item["name"] == "index")
            assert abs(index["ipd"] - .063) < .0001, index

            test.discover()
            endpoint = rf"\\.\pipe\SimXR.Control.mcp.image.{os.getpid()}"
            probe_environment = {"SIMXR_CONTROL_SOCKET": endpoint,
                "SIMXR_PROBE_FRAMES": "1800" if validate else "120000", "SIMXR_PROBE_SLEEP_MS": "1",
                "SIMXR_PROBE_STEREO_TEXTURE": "1",
                "DISABLE_XR_APILAYER_NOVENDOR_OBSMirror": "1",
                "DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY": "1"}
            if validate:
                probe_environment["SIMXR_VULKAN_VALIDATION"] = "1"
                settings = root / "vk_layer_settings.txt"
                settings.write_text("khronos_validation.validate_sync = true\n"
                    "khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG\n"
                    "khronos_validation.report_flags = error\n"
                    f"khronos_validation.log_filename = {(root / 'validation.log').as_posix()}\n", encoding="utf-8")
                probe_environment["VK_LAYER_SETTINGS_PATH"] = str(root)
            launched = client.tool("simulator_launch_application", {"executable": probe,
                "runtime_manifest": manifest, "environment": probe_environment})
            assert not launched["error"], launched
            application = launched["data"]["application_id"]

            state = test.connected(client, application, timeout=45)

            names = {item["name"] for item in client.rpc("tools/list")["result"]["tools"]}
            assert {"read_logs", "clear_logs"} <= names
            def runtime_log():
                value = client.tool("read_logs", {"application_id": application})
                if value["error"] and "Cannot open runtime log" in value["data"]["error"]:
                    return None
                assert not value["error"], value
                return value["data"] if value["data"]["available"] and value["data"]["lines"] else None
            logged = eventually(runtime_log, timeout=20)
            assert logged["runtime_process_id"] == state["runtime_process_id"] and not logged["identity_verified"]
            assert Path(logged["path"]).parent == root and logged["directory_source"] == "inferred_from_launch"
            def clear_actual_log():
                result = client.tool("clear_logs", {"application_id": application})
                if result["error"] and "Cannot open runtime log for clearing" in result["data"]["error"]:
                    return None
                assert not result["error"] and result["data"]["cleared"], result
                return result
            real_clear = eventually(clear_actual_log, timeout=20)
            assert Path(real_clear["data"]["path"]) == Path(logged["path"]) and Path(logged["path"]).is_file()
            assert not client.tool("read_logs", {"application_id": application})["error"]
            fixture = root / "logs ü"
            fixture.mkdir()
            fixture_log = fixture / ("openxr_simulator." + state["runtime_process_id"] + ".log")
            fixture_log.write_bytes(b"x" * (256 * 1024) + b"\nold\nSecond ERROR\ninvalid\xff\n")
            logs = client.tool("read_logs", {"application_id": application, "directory": str(fixture),
                "lines": 2, "filter": "error"})
            assert not logs["error"] and logs["data"]["tail_truncated"] and logs["data"]["lines"] == ["Second ERROR"], logs
            logs = client.tool("read_logs", {"application_id": application, "directory": str(fixture), "lines": 1})
            assert not logs["error"] and logs["data"]["lines"] == ["invalid\ufffd"], logs
            assert client.tool("read_logs", {"application_id": application, "lines": 4294967297})["error"]
            other_log = fixture / "openxr_simulator.123456789.log"
            other_log.write_text("other process", encoding="utf-8")
            linked_file = fixture / "hard-linked-backup"
            os.link(fixture_log, linked_file)
            assert client.tool("clear_logs", {"application_id": application, "directory": str(fixture)})["error"]
            assert fixture_log.stat().st_size > 0 and linked_file.stat().st_size > 0
            linked_file.unlink()
            cleared = client.tool("clear_logs", {"application_id": application, "directory": str(fixture)})
            assert not cleared["error"] and cleared["data"]["cleared"] and not cleared["data"]["identity_verified"], cleared
            assert fixture_log.exists() and fixture_log.stat().st_size == 0 and other_log.read_text(encoding="utf-8") == "other process"
            with fixture_log.open("ab") as stream:
                stream.write(b"new log entry\n")
            fresh = client.tool("read_logs", {"application_id": application, "directory": str(fixture)})
            assert not fresh["error"] and fresh["data"]["lines"] == ["new log entry"], fresh
            fixture_log.unlink()
            missing = client.tool("clear_logs", {"application_id": application, "directory": str(fixture)})
            assert not missing["error"] and not missing["data"]["cleared"], missing
            assert client.tool("clear_logs", {"application_id": application, "lines": 1})["error"]
            assert "analyze_stereo_consistency" in names
            def stereo_report():
                response = client.rpc("tools/call", {"name": "analyze_stereo_consistency", "arguments": {
                    "application_id": application, "frames": 1, "timeout": 20, "include_image": True}})
                if response["result"]["isError"] and "Filtered capture is unavailable" in response["result"]["content"][0]["text"]:
                    return None
                assert not response["result"]["isError"], response
                return response
            response = eventually(stereo_report, timeout=45)
            content = response["result"]["content"]
            consistency = json.loads(content[0]["text"])
            assert not consistency["incomplete"] and len(content) == 2, consistency
            with Image.open(BytesIO(base64.b64decode(content[1]["data"]))) as sheet:
                sheet.load()
                assert content[1]["mimeType"] == "image/jpeg" and sheet.width > 100 and sheet.height > 100
            sample, = consistency["samples"]
            assert sample["frame"]["generation"] == state["generation"] and len(sample["image_views"]) == 2, sample
            assert sample["frusta_source"] == "captured_image_views", sample
            expect_luau = os.environ.get("SIMXR_EXPECT_MCP_LUAU", "1") == "1"
            assert ("simulator_luau_run" in names) == expect_luau
            if expect_luau:
                script = client.tool("simulator_luau_run", {"application_id": application,
                                     "source": "print('windows rig')\nsimxr.checkpoint('ran', true)"})
                assert not script["error"], script
                run = script["data"]["run_id"]

                def script_finished():
                    status = client.tool("simulator_luau_status", {"application_id": application, "run_id": run})
                    assert not status["error"], status
                    return status["data"] if status["data"]["phase"] not in ("running", "paused") else None

                script_result = eventually(script_finished)
                assert script_result["phase"] == "complete" and script_result["output"] == ["windows rig"], script_result
                running = client.tool("simulator_luau_run", {"application_id": application,
                                      "source": "while true do end"})
                assert not running["error"], running
                run = running["data"]["run_id"]
                cancelled = client.tool("simulator_luau_cancel", {"application_id": application, "run_id": run})
                assert not cancelled["error"], cancelled
                assert eventually(script_finished)["phase"] == "cancelled"
            opened = client.tool("debug_connect", {"application_id": application})
            assert not opened["error"], opened
            connection = opened["data"]["connection"]
            def edit(tool_name, identity, **values):
                result = client.tool(tool_name, {"connection": connection, "transaction_id": str(identity), **values})
                assert not result["error"] and result["data"]["phase"] in ("queued", "rescheduled"), result
                def applied():
                    status = client.tool("simulation_status", {"connection": connection, "transaction_id": str(identity)})
                    assert not status["error"], status
                    return status["data"] if status["data"]["phase"] == "applied" else None
                eventually(applied, timeout=15)

            edit("set_head_pose", 201, position=[1, 1.7, 2], orientation=[0, 2**-.5, 0, 2**-.5])
            edit("set_controller_pose", 202, hand=0, position=[0, 1.7, 2])
            edit("set_controller_controls", 203, hand=0, trigger=0.4, grip=0.7)
            edit("set_views", 204, ipd=.08)
            edit("set_views", 205, outer_fov=[-.8726646, .6981317, .7853982, -.7853982, -.6981317, .8726646, .7853982, -.7853982])
            config = client.tool("simulation_status", {"connection": connection})
            assert not config["error"], config
            applied_state = config["data"]["document"]["configuration"]
            left = applied_state["controllers"][0]
            assert abs(left["pose"]["position"][0]) < .001 and abs(left["pose"]["position"][2] - 2) < .001, left
            assert abs(left["controls"]["trigger"] - .4) < .001 and abs(left["controls"]["grip"] - .7) < .001, left
            assert abs(applied_state["views"]["ipd"] - .08) < .0001, applied_state
            original_views = applied_state["views"]
            edit("set_headset_profile", 206, name="index")
            applied_state = client.tool("simulation_status", {"connection": connection})["data"]["document"]["configuration"]
            views = applied_state["views"]
            assert abs(views["ipd"] - .063) < .0001 and abs(views["outer_fov"][0] + .9424778) < .001, views
            assert all(views[key] == original_views[key] for key in
                       ("outer_width", "outer_height", "inset_width", "inset_height", "inset_half_angle")), views
            assert not client.tool("simulation_release_control", {"connection": connection})["error"]
            sequence_program = '{"frame":0,"head":{"x":1.2},"capture":true}\n'
            sequence_args = {"connection": connection, "program_id": "9007199254740993", "program": sequence_program}
            sequence_start = client.tool("run_pose_sequence", sequence_args)
            assert not sequence_start["error"], sequence_start
            sequence_id = sequence_start["data"]["sequence_id"]
            sequence_retry = client.tool("run_pose_sequence", sequence_args)
            assert not sequence_retry["error"] and sequence_retry["data"]["sequence_id"] == sequence_id, sequence_retry
            def sequence_capture():
                status = client.tool("get_pose_sequence", {"connection": connection, "sequence_id": sequence_id})
                assert not status["error"], status
                for receipt in status["data"]["document"]["captures"]:
                    if receipt["phase"] == "ready":
                        return receipt
                return None
            receipt = eventually(sequence_capture, timeout=20)
            sequence_image = client.tool("debug_retain_image", {"connection": connection, "capture": receipt["capture_id"]})
            assert not sequence_image["error"], sequence_image
            released = client.tool("release_pose_sequence_capture", {"connection": connection, "capture": receipt["capture_id"]})
            assert not released["error"] and released["data"]["released"], released
            retained_info = client.tool("debug_image_info", {"connection": connection, "image": sequence_image["data"]["image"]})
            assert not retained_info["error"] and retained_info["data"]["frame"]["frame_id"] == receipt["frame"], retained_info
            assert not client.tool("debug_release_image", {"connection": connection, "image": sequence_image["data"]["image"]})["error"]
            assert not client.tool("simulation_release_control", {"connection": connection})["error"]
            sequence_root = root / "序列捕获"
            sequence_root.mkdir()
            for oversized in (4294967297, 18446744073709551615):
                invalid = client.tool("start_pose_sequence_capture", {"job_id": "windows-sequence", "connection": connection,
                    "program_id": "9007199254740994", "program": '{"frame":0,"head":{"x":1.3},"capture":true}\n',
                    "directory": str(sequence_root), "timeout": 20, "max_captures": oversized})
                assert invalid["error"] and not list(sequence_root.iterdir()), invalid
            persistent = client.tool("start_pose_sequence_capture", {"job_id": "windows-sequence", "connection": connection,
                "program_id": "9007199254740994", "program": '{"frame":0,"head":{"x":1.3},"capture":true}\n',
                "directory": str(sequence_root), "timeout": 20})
            assert not persistent["error"], persistent
            def sequence_completed():
                value = client.tool("get_pose_sequence_capture", {"job_id": "windows-sequence"})
                assert not value["error"] and value["data"]["phase"] not in ("failed", "cancelled"), value
                return value["data"] if value["data"]["phase"] == "complete" else None
            persistent = eventually(sequence_completed, timeout=30)["capture"]
            assert len(persistent["captures"]) == 1, persistent
            item, = persistent["captures"]
            assert item["frame_key"]["generation"] == state["generation"], item
            with Image.open(item["path"]) as stereo:
                assert stereo.width >= 128 and stereo.width % 2 == 0 and stereo.height >= 48, stereo.size
                stereo.load()
            manifest = Path(persistent["directory"]) / "manifest.json"
            assert json.loads(manifest.read_text(encoding="utf-8")) == persistent
            assert not client.tool("simulation_release_control", {"connection": connection})["error"]
            diagnostic = client.tool("get_diagnostics", {"application_id": application})
            assert not diagnostic["error"] and diagnostic["data"]["generation"] == state["generation"], diagnostic
            assert "events" in diagnostic["data"]["diagnostics"] and "logs_unavailable" in diagnostic["data"]
            projection = client.tool("get_projection_log", {"application_id": application, "max_entries": 1})
            assert not projection["error"] and projection["data"]["projection"]["generation"] == state["generation"], projection
            def history_ready():
                value = client.tool("debug_image_history", {"connection": connection, "target": "composed_eyes", "count": 1})
                return not value["error"] and bool(value["data"]["document"]["entries"])
            eventually(history_ready, timeout=20)
            monitor = {"monitor_id": "windows-monitor", "application_id": application,
                       "target": "composed", "timeout": 25}
            started_monitor = client.tool("start_flicker_monitor", monitor)
            assert not started_monitor["error"], started_monitor

            def monitor_ready():
                status = client.tool("get_flicker_status", {"application_id": application})
                assert not status["error"] and status["data"]["phase"] != "failed", status
                return status["data"] if status["data"]["windows"] else None

            monitor_status = eventually(monitor_ready, timeout=40)
            assert monitor_status["generation"] == state["generation"] and monitor_status["frames_observed"] >= 2
            assert not client.tool("stop_flicker_monitor", {"application_id": application, "monitor_id": "windows-monitor"})["error"]
            for incident_tool in ("capture_flicker_window", "capture_ui_flicker_window"):
                incident_dir = root / ("bewijs ü " + incident_tool)
                arguments = {"application_id": application, "timeout": 30, "max_frames": 4, "analyze": True}
                if incident_tool == "capture_flicker_window":
                    arguments["directory"] = str(incident_dir)
                def capture_incident():
                    value = client.rpc("tools/call", {"name": incident_tool, "arguments": arguments})["result"]
                    if value["isError"] and "Incident contains unavailable frame" in value["content"][0]["text"]:
                        return None
                    return value
                incident = eventually(capture_incident, timeout=45)
                assert not incident["isError"], incident
                incident_info = json.loads(incident["content"][0]["text"])
                assert incident_info["generation"] == state["generation"] and len(incident_info["selected_frames"]) == 4, incident
                assert incident_info["window"]["post_count"] == 12
                analysis = incident_info["analysis"]
                assert analysis["frames_analyzed"] + analysis["frames_skipped"] == len(incident_info["window"]["entries"])
                if "directory" in arguments:
                    assert json.loads((incident_dir / "manifest.json").read_text(encoding="utf-8")) == incident_info
                    assert len(incident_info["saved_frames"]) == len(incident_info["window"]["entries"])
                    assert (incident_dir / "contact.jpg").read_bytes() == base64.b64decode(incident["content"][1]["data"], validate=True)
                    for frame in incident_info["saved_frames"]:
                        with Image.open(incident_dir / frame["file"]) as decoded_frame:
                            assert decoded_frame.size == (320, 104) and decoded_frame.format == "PNG"
                            decoded_frame.load()
                with Image.open(BytesIO(base64.b64decode(incident["content"][1]["data"], validate=True))) as decoded:
                    assert decoded.size == (640, 208), decoded.size
                    decoded.load()
            acquired = client.tool("debug_acquire", {"connection": connection})
            assert not acquired["error"], acquired
            lease = acquired["data"]["lease"]
            next_id = 1

            def operation(identity):
                value = client.tool("debug_operation", {"connection": connection, "operation": identity})
                assert not value["error"], value
                return value["data"] if value["data"]["phase"] not in ("queued", "pending") else None

            pause = client.tool("debug_pause", {"connection": connection, "lease": lease, "command_id": str(next_id)})
            assert not pause["error"], pause
            next_id += 1
            paused = eventually(lambda: operation(pause["data"]["operation_id"]))
            for capture in dict.fromkeys(paused["captures"]):
                release = client.tool("debug_release", {"connection": connection, "lease": lease,
                                      "command_id": str(next_id), "capture": capture})
                assert not release["error"], release
                next_id += 1
                eventually(lambda: operation(release["data"]["operation_id"]))
            for _ in range(8):
                step = client.tool("debug_step", {"connection": connection, "lease": lease,
                                   "command_id": str(next_id), "lookahead": 1})
                assert not step["error"], step
                next_id += 1
                captured = eventually(lambda: operation(step["data"]["operation_id"]))
                if captured.get("progress") == "snapshot_ready":
                    break
                for capture in dict.fromkeys(captured.get("captures", [])):
                    release = client.tool("debug_release", {"connection": connection, "lease": lease,
                                          "command_id": str(next_id), "capture": capture})
                    assert not release["error"], release
                    next_id += 1
                    eventually(lambda: operation(release["data"]["operation_id"]))
            else:
                raise AssertionError("No GPU snapshot became ready")
            retained = client.tool("debug_retain_image", {"connection": connection, "capture": captured["capture_id"]})
            assert not retained["error"], retained
            image = retained["data"]["image"]

            def image_ready():
                value = client.tool("debug_image_info", {"connection": connection, "image": image})
                assert not value["error"], value
                return value["data"] if value["data"]["phase"] == "complete" else None

            description = eventually(image_ready)["image_description"]
            assert description["width"] >= 64 and description["height"] >= 48
            def read_original(x, y):
                selection = {"x": x, "y": y, "width": 63, "height": 47, "output_width": 63, "output_height": 47}
                started = client.tool("debug_start_export", {"connection": connection, "image": image,
                                      "layer": 0, "region": selection})
                assert not started["error"], started
                original_ticket = started["data"]["export"]
                def ready():
                    value = client.tool("debug_export_info", {"connection": connection, "export": original_ticket})
                    assert not value["error"] and value["data"]["phase"] != "failed", value
                    return value["data"] if value["data"]["phase"] == "complete" else None
                original_info = eventually(ready)
                assert int(original_info["total_bytes"]) == 63 * 47 * 4, original_info
                raw = client.tool("debug_read_export", {"connection": connection, "export": original_ticket,
                                  "length": int(original_info["total_bytes"])})
                assert not raw["error"], raw
                assert not client.tool("debug_release_export", {"connection": connection, "export": original_ticket})["error"]
                return original_info, base64.b64decode(raw["data"]["bytes"], validate=True)
            original = None
            for y_fraction in (0.25, 0.5, 0.75):
                for x_fraction in (0.2, 0.4, 0.5, 0.6, 0.8):
                    x = max(0, min(description["width"] - 63, int(description["width"] * x_fraction) - 31))
                    y = max(0, min(description["height"] - 47, int(description["height"] * y_fraction) - 23))
                    original_info, candidate = read_original(x, y)
                    pixels = {candidate[i:i+4] for i in range(0, len(candidate), 4)}
                    selected = b"".join(candidate[(((2*row+1)*47//48)*63+((2*column+1)*63//64))*4:
                                                  (((2*row+1)*47//48)*63+((2*column+1)*63//64))*4+4]
                                        for row in range(24) for column in range(32))
                    shifted = b"".join(candidate[(((2*row+1)*47//48)*63+min(62, ((2*column+1)*63//64)+1))*4:
                                                 (((2*row+1)*47//48)*63+min(62, ((2*column+1)*63//64)+1))*4+4]
                                       for row in range(24) for column in range(32))
                    if len(pixels) > 1 and selected != shifted:
                        region = {"x": x, "y": y, "width": 63, "height": 47, "output_width": 32, "output_height": 24}
                        original = candidate
                        break
                if original is not None:
                    break
            assert original is not None, "Could not find spatial variation in the retained image"
            started = client.tool("debug_start_export", {"connection": connection, "image": image,
                                  "layer": 0, "region": region})
            assert not started["error"], started
            ticket = started["data"]["export"]

            def export_ready():
                value = client.tool("debug_export_info", {"connection": connection, "export": ticket})
                assert not value["error"] and value["data"]["phase"] != "failed", value
                return value["data"] if value["data"]["phase"] == "complete" else None

            info = eventually(export_ready)
            assert info["region"] == region and info["image_description"]["width"] == 32
            png_pixels = None
            for image_format in ("png", "jpeg"):
                result = client.rpc("tools/call", {"name": "debug_render_export", "arguments":
                                    {"connection": connection, "export": ticket, "format": image_format}})
                assert "result" in result and not result["result"]["isError"], result
                content = result["result"]["content"]
                assert content[1]["type"] == "image" and content[1]["mimeType"] == f"image/{image_format}"
                with Image.open(BytesIO(base64.b64decode(content[1]["data"], validate=True))) as decoded:
                    assert decoded.size == (32, 24)
                    decoded.load()
                    if image_format == "png":
                        png_pixels = decoded.convert("RGBA").tobytes()
            artifact = client.rpc("tools/call", {"name": "debug_export_artifact", "arguments":
                                  {"connection": connection, "export": ticket}})
            assert not artifact["result"]["isError"], artifact
            url = urlsplit(artifact["result"]["content"][1]["uri"])
            download_connection = http.client.HTTPConnection(url.hostname, url.port, timeout=30)
            download_connection.request("GET", url.path, headers={"Authorization": "Bearer " + test.discover()["token"]})
            response = download_connection.getresponse()
            artifact_data = response.read()
            assert response.status == 200 and len(artifact_data) == int(info["total_bytes"])
            download_connection.close()
            raw = client.tool("debug_read_export", {"connection": connection, "export": ticket,
                              "length": len(artifact_data)})
            assert not raw["error"] and base64.b64decode(raw["data"]["bytes"]) == artifact_data
            pixel_format = info["image_description"]["format"]
            assert pixel_format in (37, 43, 44, 50), pixel_format
            if pixel_format in (44, 50):
                rgba = bytes(value for offset in range(0, len(artifact_data), 4)
                             for value in (artifact_data[offset + 2], artifact_data[offset + 1],
                                           artifact_data[offset], artifact_data[offset + 3]))
            else:
                rgba = artifact_data
            assert png_pixels == rgba, "Encoded PNG differs from the exact raw export"
            image_path = root / "画像-左.png"
            image_args = {"connection": connection, "export": ticket, "path": str(image_path)}
            saved_image = client.tool("debug_save_image", image_args)
            assert not saved_image["error"] and not saved_image["data"]["lossy"], saved_image
            with Image.open(image_path) as saved_png:
                assert saved_png.convert("RGBA").tobytes() == rgba, "Saved PNG differs from raw export"
            previous_image = image_path.read_bytes()
            duplicate_image = client.tool("debug_save_image", image_args)
            assert duplicate_image["error"] and image_path.read_bytes() == previous_image, duplicate_image
            replaced_image = client.tool("debug_save_image", dict(image_args, overwrite=True))
            assert not replaced_image["error"] and image_path.read_bytes() == previous_image, replaced_image
            jpg_path = root / "画像-左.jpg"
            jpeg_image = client.tool("debug_save_image", {"connection": connection, "export": ticket,
                                     "path": str(jpg_path), "quality": 90})
            assert not jpeg_image["error"] and jpeg_image["data"]["lossy"], jpeg_image
            with Image.open(jpg_path) as saved_jpeg:
                assert saved_jpeg.size == (32, 24), saved_jpeg.size
            assert not client.tool("debug_release_export", {"connection": connection, "export": ticket})["error"]
            assert original_info["image_description"]["format"] == pixel_format
            expected = b"".join(original[(((2*y+1)*47//48)*63+((2*x+1)*63//64))*4:
                                        (((2*y+1)*47//48)*63+((2*x+1)*63//64))*4+4]
                                for y in range(24) for x in range(32))
            assert len({expected[i:i+4] for i in range(0, len(expected), 4)}) > 1, "Reduced region lost all source variation"
            assert artifact_data == expected, "GPU crop/resize did not select the expected source pixels"
            assert not client.tool("debug_release_image", {"connection": connection, "image": image})["error"]
            frame = captured["frame"]["frame_id"]
            video = client.tool("debug_start_video_export", {"connection": connection,
                                "first_frame": frame, "last_frame": frame})
            assert not video["error"], video
            video_ticket = video["data"]["export"]

            def video_ready():
                value = client.tool("debug_export_info", {"connection": connection, "export": video_ticket})
                assert not value["error"] and value["data"]["phase"] != "failed", value
                return value["data"] if value["data"]["phase"] == "complete" else None

            video_info = eventually(video_ready, timeout=45)
            assert video_info["media_type"] == "video/mp4", video_info
            video_link = client.rpc("tools/call", {"name": "debug_export_artifact", "arguments":
                                    {"connection": connection, "export": video_ticket}})
            assert not video_link["result"]["isError"], video_link
            item = video_link["result"]["content"][1]
            assert item["type"] == "resource_link" and item["mimeType"] == "video/mp4", item
            video_url = urlsplit(item["uri"])
            download_connection = http.client.HTTPConnection(video_url.hostname, video_url.port, timeout=30)
            download_connection.request("GET", video_url.path,
                                        headers={"Authorization": "Bearer " + test.discover()["token"]})
            response = download_connection.getresponse()
            movie = response.read()
            assert response.status == 200 and len(movie) == int(video_info["total_bytes"])
            download_connection.close()
            clip = root / "clip.mp4"
            clip.write_bytes(movie)
            streams = json.loads(subprocess.check_output([os.environ.get("SIMXR_FFPROBE", "ffprobe"),
                                 "-v", "error", "-show_streams",
                                 "-of", "json", str(clip)], text=True))["streams"]
            assert len(streams) == 1 and streams[0]["codec_name"] == "h264", streams
            assert not client.tool("debug_release_export", {"connection": connection, "export": video_ticket})["error"]
            target = root / "stereo-影片.mp4"
            save_args = {"connection": connection, "first_frame": frame, "last_frame": frame,
                         "session": state["session_id"], "generation": state["generation"],
                         "path": str(target), "timeout": 30}
            saved = client.tool("debug_save_video", save_args)
            assert not saved["error"] and target.is_file(), saved
            first = target.read_bytes()
            assert len(first) == int(saved["data"]["bytes"]), saved
            streams = json.loads(subprocess.check_output([os.environ.get("SIMXR_FFPROBE", "ffprobe"),
                "-v", "error", "-show_streams", "-of", "json", str(target)], text=True))["streams"]
            assert len(streams) == 1 and streams[0]["codec_name"] == "h264", streams
            duplicate = client.tool("debug_save_video", save_args)
            assert duplicate["error"] and target.read_bytes() == first, duplicate
            replaced = client.tool("debug_save_video", dict(save_args, overwrite=True))
            assert not replaced["error"] and target.is_file(), replaced
            protected = target.read_bytes()
            for _ in range(4):
                second = client.tool("debug_connect", {"application_id": application})
                assert not second["error"], second
                second_connection = second["data"]["connection"]
                timed_out = client.tool("debug_save_video", dict(save_args, connection=second_connection,
                                        overwrite=True, timeout=0.001))
                assert timed_out["error"] and target.read_bytes() == protected, timed_out
                assert client.tool("debug_status", {"connection": second_connection})["error"]
            assert not client.tool("debug_status", {"connection": connection})["error"]
            assert not list(root.glob("stereo-影片.mp4.*")), "Temporary files were not retired"
            if validate:
                renewed = client.tool("debug_renew", {"connection": connection, "lease": lease})
                assert not renewed["error"], renewed
                resumed = client.tool("debug_resume", {"connection": connection, "lease": lease,
                                      "command_id": str(next_id)})
                assert not resumed["error"], resumed
                result = eventually(lambda: operation(resumed["data"]["operation_id"]))
                assert result["phase"] == "complete", result
                assert not client.tool("debug_disconnect", {"connection": connection})["error"]
                def exited():
                    status = client.tool("simulator_application_status", {"application_id": application})
                    assert not status["error"], status
                    return status["data"] if status["data"]["launch_state"] == "exited" else None
                status = eventually(exited, timeout=90)
                assert status["exit_code"] == 0, status
                assert (root / "validation.log").is_file(), "Validation layer settings were not loaded"
                validation_messages = (root / "validation.log").read_text(encoding="utf-8", errors="replace")
                assert not validation_messages.strip(), validation_messages
            assert not client.tool("simulator_shutdown", {})["error"]
            bridge.stdin.close()
            assert bridge.wait(timeout=15) == 0
            print("PASS Windows real Vulkan XR MCP image export, MP4 download and atomic video save" +
                  (" with inspection validation and normal probe teardown" if validate else ""))
        finally:
            if validate and os.environ.get("SIMXR_VALIDATION_LOG"):
                validation_log = root / "validation.log"
                if validation_log.exists():
                    Path(os.environ["SIMXR_VALIDATION_LOG"]).write_bytes(validation_log.read_bytes())


if __name__ == "__main__":
    main()
