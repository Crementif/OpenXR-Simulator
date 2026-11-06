#!/usr/bin/env python3
"""Exercise the windowless host through the real native connector and XR probe."""

import base64
from email.parser import BytesParser
import http.client
from io import BytesIO
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from urllib.parse import urlsplit
from PIL import Image

from mcp_support import Client, discovery_record, eventually

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "sdk/python"))
from simxr_control import Client as NativeClient, decode_command
from simxr_control.body_observation import JOINT_SETS


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="simxr-mcp-windowless-") as temporary:
        root = Path(temporary)
        discovery = root / "discovery"
        env = dict(os.environ, SIMXR_MCP_DISCOVERY_DIR=str(discovery), SIMXR_DATA_DIR=temporary,
                   SIMXR_COMPANION_SETTINGS=str(root / "settings"), SIMXR_DETERMINISTIC="0",
                   SDL_VIDEODRIVER="simxr-unavailable-video-driver", PATH="")
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "XR_RUNTIME_JSON", "SIMXR_MCP_TOKEN"):
            env.pop(key, None)
        app_pid = child_pid = None
        other_bridge = None
        with (root / "bridge.log").open("wb") as log:
            bridge = subprocess.Popen([connector, "--app", app, "--windowless"], env=env,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                      text=True, bufsize=1, start_new_session=True)
            try:
                client = Client(bridge)
                result = client.initialize('test')
                assert "result" in result, result

                names = {tool["name"] for tool in client.rpc("tools/list")["result"]["tools"]}
                assert "simulator_shutdown" in names
                expect_luau = os.environ.get("SIMXR_EXPECT_MCP_LUAU", "1") == "1"
                assert ("simulator_luau_run" in names) == expect_luau

                app_pid = int(eventually(lambda: discovery_record(discovery))["process_id"])
                assert b"--windowless" in Path(f"/proc/{app_pid}/cmdline").read_bytes().split(b"\0")
                maps = Path(f"/proc/{app_pid}/maps").read_text()
                assert "libvulkan_lvp" not in maps and "libvulkan_radeon" not in maps
                launched = client.tool("simulator_launch_application", {
                    "executable": probe, "environment": {"SIMXR_PROBE_FRAMES": "120000",
                                                          "SIMXR_PROBE_INTERACTIVE": "1"}})
                assert not launched["error"], launched
                application_id = launched["data"]["application_id"]

                def connected():
                    status = client.tool("simulator_application_status", {"application_id": application_id})
                    assert not status["error"], status
                    return status["data"] if status["data"]["session_running"] else None

                state = eventually(connected, timeout=30)
                child_pid = int(state["process_id"])
                assert int(state["session_id"]) and int(state["generation"])
                opened = client.tool("debug_connect", {"application_id": application_id})
                assert not opened["error"], opened
                debug = opened["data"]["connection"]
                assert opened["data"]["session"] == state["session_id"]
                common = {"connection": debug, "session": opened["data"]["session"],
                          "generation": opened["data"]["generation"]}
                status = client.tool("debug_status", common)
                assert not status["error"] and status["data"]["running"], status
                wrong = dict(common, generation=str(int(common["generation"]) + 1))
                assert client.tool("debug_status", wrong)["error"]
                acquired = client.tool("debug_acquire", {"connection": debug})
                assert not acquired["error"], acquired
                lease = acquired["data"]["lease"]
                other_bridge = subprocess.Popen([connector, "--app", app, "--windowless"], env=env,
                                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                                text=True, bufsize=1, start_new_session=True)
                other = Client(other_bridge)
                init = other.initialize('other')
                assert "result" in init, init

                assert other.tool("debug_status", {"connection": debug})["error"]
                other_open = other.tool("debug_connect", {"application_id": application_id})
                assert not other_open["error"], other_open
                other_debug = other_open["data"]["connection"]
                assert other.tool("debug_acquire", {"connection": other_debug})["error"]

                def operation(identity):
                    value = client.tool("debug_operation", {"connection": debug, "operation": identity})
                    assert not value["error"], value
                    return value["data"] if value["data"].get("phase") not in ("queued", "pending") else None

                pause = client.tool("debug_pause", {"connection": debug, "lease": lease, "command_id": "1"})
                assert not pause["error"], pause
                paused = eventually(lambda: operation(pause["data"]["operation_id"]))
                assert paused["progress"] == "paused" and isinstance(paused["captures"], list), paused
                initial = client.tool("simulation_status", {"connection": debug})
                assert not initial["error"] and "frame_id" in initial["data"]["document"], initial
                frame = int(initial["data"]["document"]["frame_id"]) + 1
                started_recording = client.tool("simulation_start_recording", {"connection": debug, "drain": True})
                assert not started_recording["error"], started_recording
                recording_id = started_recording["data"]["document"]["recording_id"]
                transaction = {"connection": debug, "transaction_id": "9007199254740999", "frame": str(frame),
                               "commands": [{"kind": "take_control", "channel": "head"},
                                            {"kind": "head_pose", "position": [2, 1.8, -3]},
                                            {"kind": "take_control", "channel": "left_controls"},
                                            {"kind": "controller_controls", "hand": 0, "trigger": 0.75},
                                            {"kind": "take_control", "channel": "body"},
                                            {"kind": "body", "preset": 2, "confidence": 0.75,
                                             "controls": [{"offset": [0.1, 0, 0]}, {}, {}, {}, {}],
                                             "joints": [{"joint": 2, "tracked": False}]},
                                            {"kind": "take_control", "channel": "trackers"},
                                            {"kind": "trackers", "devices": [
                                                {"identity": "9007199254740993", "role": 2,
                                                 "anchor": 5, "source_path": "test:/left-foot",
                                                 "joint_offset": {"position": [0.2, 0, 0]}}]},
                                            {"kind": "take_control", "channel": "session"},
                                            {"kind": "reference_spaces", "local_position": [0.25, 0, 0],
                                             "stage_position": [0, 0, -0.5], "stage_bounds": [3, 4]},
                                            {"kind": "take_control", "channel": "views"},
                                            {"kind": "eye_views", "space_revision": "9007199254740993",
                                             "left": {"position": [-0.03, 1.7, 0], "fov": [-0.8, 0.9, 0.7, -0.6]},
                                             "right": {"position": [0.03, 1.7, 0], "fov": [-0.9, 0.8, 0.7, -0.6]}}]}
                queued = client.tool("simulation_schedule", transaction)
                assert not queued["error"] and queued["data"]["phase"] == "queued", queued
                duplicate_tx = client.tool("simulation_schedule", transaction)
                assert not duplicate_tx["error"] and duplicate_tx["data"]["transaction_id"] == transaction["transaction_id"]
                changed_tx = dict(transaction, commands=[{"kind": "head_pose", "position": [3, 1, 0]}])
                conflict = client.tool("simulation_schedule", changed_tx)
                assert not conflict["error"] and conflict["data"]["phase"] == "conflict", conflict
                isolated = other.tool("simulation_schedule", {"connection": other_debug,
                                      "transaction_id": transaction["transaction_id"], "frame": "5000",
                                      "commands": [{"kind": "take_control", "channel": "gaze"}]})
                assert not isolated["error"] and isolated["data"]["phase"] == "queued", isolated
                assert not other.tool("simulation_cancel", {"connection": other_debug,
                                      "transaction_id": transaction["transaction_id"]})["error"]
                invalid_tx = dict(transaction, transaction_id="9007199254741000",
                                  commands=[{"kind": "head_pose", "positon": [1, 2, 3]}])
                assert client.tool("simulation_schedule", invalid_tx)["error"]
                next_command = 9007199254740993
                for capture_id in dict.fromkeys(paused["captures"]):
                    released = client.tool("debug_release", {"connection": debug, "lease": lease,
                                           "command_id": str(next_command), "capture": capture_id})
                    assert not released["error"], released
                    eventually(lambda: operation(released["data"]["operation_id"]))
                    next_command += 1
                saw_lookahead = False
                checked_export = False
                for _ in range(8):
                    release_inspection = None
                    step_args = {"connection": debug, "lease": lease, "command_id": str(next_command), "lookahead": 3}
                    step = client.tool("debug_step", step_args)
                    duplicate = client.tool("debug_step", step_args)
                    assert not step["error"] and not duplicate["error"] and duplicate["data"]["duplicate"], (
                        step, duplicate, client.tool("simulator_application_status", {"application_id": application_id}))
                    assert duplicate["data"]["operation_id"] == step["data"]["operation_id"]
                    next_command += 1
                    capture = eventually(lambda: operation(step["data"]["operation_id"]))
                    assert capture["progress"] == "snapshot_ready", capture
                    assert capture["command_id"] == step_args["command_id"], capture
                    retained = client.tool("debug_status", {"connection": debug})
                    assert capture["capture_id"] in retained["data"]["retention"]["captures"], retained
                    metadata = client.tool("debug_capture", {"connection": debug, "capture": capture["capture_id"]})
                    assert not metadata["error"] and metadata["data"]["image_available"], metadata
                    if not checked_export:
                        inspection_args = {"connection": debug, "capture": capture["capture_id"]}
                        inspection = client.tool("debug_inspect", inspection_args)
                        assert not inspection["error"], inspection
                        document = inspection["data"]["document"]
                        assert document["session"] == opened["data"]["session"]
                        assert document["generation"] == opened["data"]["generation"]
                        assert document["frame_id"] == metadata["data"]["frame"]["frame_id"]
                        repeated = client.tool("debug_inspect", inspection_args)
                        assert not repeated["error"] and repeated["data"]["inspection"] == inspection["data"]["inspection"]
                        revision = document["view_settings"]
                        assert isinstance(revision, str) and int(revision) > 0, document
                        assert other.tool("debug_revision", {"connection": other_debug, "revision": revision})["error"]
                        feature = client.tool("debug_revision", {"connection": debug, "revision": revision})
                        assert not feature["error"] and feature["data"]["kind"] == "view_settings_v1", feature
                        assert isinstance(feature["data"]["document"], dict), feature
                        release_inspection = {"connection": debug, "inspection": inspection["data"]["inspection"]}
                        retained_image = client.tool("debug_retain_image", {"connection": debug,
                                                     "capture": capture["capture_id"]})
                        assert not retained_image["error"], retained_image
                        image = retained_image["data"]["image"]
                        assert other.tool("debug_image_info", {"connection": other_debug, "image": image})["error"]

                        def image_ready():
                            info = client.tool("debug_image_info", {"connection": debug, "image": image})
                            assert not info["error"], info
                            return info["data"] if info["data"]["phase"] == "complete" else None

                        image_info = eventually(image_ready)
                        assert image_info["frame"]["frame_id"] == metadata["data"]["frame"]["frame_id"]
                        export = client.tool("debug_start_export", {"connection": debug, "image": image, "layer": 0})
                        assert not export["error"], export
                        ticket = export["data"]["export"]
                        assert not client.tool("debug_release_image", {"connection": debug, "image": image})["error"]

                        def export_ready():
                            info = client.tool("debug_export_info", {"connection": debug, "export": ticket})
                            assert not info["error"] and info["data"]["phase"] != "failed", info
                            return info["data"] if info["data"]["phase"] == "complete" else None

                        exported = eventually(export_ready)
                        assert int(exported["total_bytes"]) >= 4096 and exported["image_description"]["width"] > 0
                        artifact = client.rpc("tools/call", {"name": "debug_export_artifact", "arguments":
                                              {"connection": debug, "export": ticket}})
                        assert "result" in artifact and not artifact["result"]["isError"], artifact
                        link = artifact["result"]["content"][1]
                        assert link["type"] == "resource_link" and link["mimeType"] == "application/octet-stream"
                        duplicate_link = client.rpc("tools/call", {"name": "debug_export_artifact", "arguments":
                                                    {"connection": debug, "export": ticket}})
                        assert duplicate_link["result"]["content"][1]["uri"] == link["uri"]
                        url = urlsplit(link["uri"])
                        download_record = discovery_record(discovery)
                        assert url.hostname == "127.0.0.1" and url.port == download_record["port"]

                        def download(headers, selected=url, response_headers=None):
                            connection = http.client.HTTPConnection(selected.hostname, selected.port, timeout=30)
                            connection.request("GET", selected.path, headers=headers)
                            response = connection.getresponse()
                            status, body = response.status, response.read()
                            if response_headers is not None:
                                response_headers.update(dict(response.getheaders()))
                            connection.close()
                            return status, body

                        authorization = {"Authorization": "Bearer " + download_record["token"]}
                        assert download({})[0] == 401
                        assert download(dict(authorization, Origin="https://attacker.example"))[0] == 403
                        assert download(dict(authorization, **{"MCP-Session-Id": "wrong"}))[0] == 404
                        other_image = other.tool("debug_retain_image", {"connection": other_debug,
                                                 "capture": capture["capture_id"]})
                        assert not other_image["error"], other_image
                        other_image = other_image["data"]["image"]
                        small_region = {"x": 0, "y": 0, "width": 8, "height": 8,
                                        "output_width": 8, "output_height": 8}
                        other_export = other.tool("debug_start_export", {"connection": other_debug, "image": other_image,
                                                  "layer": 0, "region": small_region})
                        assert not other_export["error"], other_export
                        other_ticket = other_export["data"]["export"]

                        def other_export_ready():
                            result = other.tool("debug_export_info", {"connection": other_debug, "export": other_ticket})
                            assert not result["error"] and result["data"]["phase"] != "failed", result
                            return result["data"] if result["data"]["phase"] == "complete" else None

                        eventually(other_export_ready)
                        other_artifact = other.rpc("tools/call", {"name": "debug_export_artifact", "arguments":
                                                   {"connection": other_debug, "export": other_ticket}})
                        assert not other_artifact["result"]["isError"], other_artifact
                        other_url = urlsplit(other_artifact["result"]["content"][1]["uri"])
                        assert download(authorization, other_url)[0] == 200
                        chunk = client.tool("debug_read_export", {"connection": debug, "export": ticket, "length": 4096})
                        assert not chunk["error"] and len(base64.b64decode(chunk["data"]["bytes"], validate=True)) == 4096, chunk
                        assert not chunk["data"]["end"]
                        with NativeClient(state["endpoint"]) as observer:
                            native_session, = observer.sessions()
                            native_image = observer.retain_image(native_session, int(capture["capture_id"]))["image_reader_id"]
                            native_export = observer.start_export(native_image, layer=0)["export_id"]

                            def native_ready():
                                info = observer.export_info(native_export)
                                assert info["phase"] != "failed", info
                                return info if info["phase"] == "complete" else None

                            native_info = eventually(native_ready)
                            total = int(exported["total_bytes"])
                            assert native_info["total_bytes"] == total
                            full_pixels = bytearray()
                            offset = 0
                            while offset < total:
                                length = min(600001, total - offset)
                                raw = client.tool("debug_read_export", {"connection": debug, "export": ticket,
                                                  "offset": str(offset), "length": length})
                                assert not raw["error"], raw
                                pixels = base64.b64decode(raw["data"]["bytes"], validate=True)
                                expected = observer.read_export(native_export, offset, length)
                                assert pixels == expected["bytes"] and len(pixels) == length
                                full_pixels.extend(pixels)
                                assert int(raw["data"]["offset"]) == offset and int(raw["data"]["length"]) == length
                                offset += length
                                assert raw["data"]["end"] == (offset == total)
                            response_headers = {}
                            status, part = download(dict(authorization, Range="bytes=13-75"), response_headers=response_headers)
                            assert status == 206 and part == full_pixels[13:76]
                            assert response_headers["Content-Range"] == f"bytes 13-75/{total}"
                            status, tail = download(dict(authorization, Range="bytes=-17"))
                            assert status == 206 and tail == full_pixels[-17:]
                            status, tail = download(dict(authorization, Range=f"bytes={total - 21}-"))
                            assert status == 206 and tail == full_pixels[-21:]
                            response_headers = {}
                            status, multipart = download(dict(authorization, Range="bytes=0-3,10-14"),
                                                         response_headers=response_headers)
                            assert status == 206
                            message = BytesParser().parsebytes(("MIME-Version: 1.0\r\nContent-Type: " +
                                      response_headers["Content-Type"] + "\r\n\r\n").encode() + multipart)
                            parts = message.get_payload()
                            assert len(parts) == 2 and [part.get_payload(decode=True) for part in parts] == [
                                full_pixels[:4], full_pixels[10:15]]
                            assert download(dict(authorization, Range=f"bytes={total}-"))[0] == 416
                            status, downloaded = download(authorization)
                            assert status == 200 and downloaded == full_pixels
                            source = exported["image_description"]
                            pixel_bytes = {37: 4, 43: 4, 44: 4, 50: 4, 64: 4, 97: 8, 109: 16}[source["format"]]
                            assert source["width"] >= 69 and source["height"] >= 55
                            region_image = client.tool("debug_retain_image", {"connection": debug,
                                                       "capture": capture["capture_id"]})
                            assert not region_image["error"], region_image
                            region_image = region_image["data"]["image"]
                            for output_width, output_height in ((64, 48), (32, 24)):
                                region = {"x": 5, "y": 7, "width": 64, "height": 48,
                                          "output_width": output_width, "output_height": output_height}
                                reduced = client.tool("debug_start_export", {"connection": debug, "image": region_image,
                                                      "layer": 0, "region": region})
                                assert not reduced["error"], reduced
                                reduced_ticket = reduced["data"]["export"]

                                def region_ready():
                                    result = client.tool("debug_export_info", {"connection": debug, "export": reduced_ticket})
                                    assert not result["error"] and result["data"]["phase"] != "failed", result
                                    return result["data"] if result["data"]["phase"] == "complete" else None

                                reduced_info = eventually(region_ready)
                                assert reduced_info["region"] == region and reduced_info["layer"] == 0, reduced_info
                                assert reduced_info["image_description"]["width"] == output_width
                                assert reduced_info["image_description"]["height"] == output_height
                                reduced_bytes = int(reduced_info["total_bytes"])
                                assert reduced_bytes == output_width * output_height * pixel_bytes
                                raw = client.tool("debug_read_export", {"connection": debug, "export": reduced_ticket,
                                                  "length": reduced_bytes})
                                assert not raw["error"] and raw["data"]["end"], raw
                                actual = base64.b64decode(raw["data"]["bytes"], validate=True)
                                expected = bytearray()
                                for y in range(output_height):
                                    source_y = 7 + ((2 * y + 1) * 48 // (2 * output_height))
                                    for x in range(output_width):
                                        source_x = 5 + ((2 * x + 1) * 64 // (2 * output_width))
                                        index = (source_y * source["width"] + source_x) * pixel_bytes
                                        expected.extend(full_pixels[index:index + pixel_bytes])
                                assert actual == expected, "GPU crop or resize changed selected pixels"
                                for image_format, media_type in (("png", "image/png"), ("jpeg", "image/jpeg")):
                                    rendered = client.rpc("tools/call", {"name": "debug_render_export",
                                                          "arguments": {"connection": debug, "export": reduced_ticket,
                                                                        "format": image_format, "quality": 85}})
                                    assert "result" in rendered and not rendered["result"]["isError"], rendered
                                    content = rendered["result"]["content"]
                                    info = json.loads(content[0]["text"])
                                    assert content[1]["type"] == "image" and content[1]["mimeType"] == media_type
                                    assert info["frame"] == image_info["frame"] and info["region"] == region
                                    encoded = base64.b64decode(content[1]["data"], validate=True)
                                    assert info["encoded_bytes"] == len(encoded) and info["lossy"] == (image_format == "jpeg")
                                    repeated = client.rpc("tools/call", {"name": "debug_render_export",
                                        "arguments": {"connection": debug, "export": reduced_ticket,
                                                      "format": image_format, "quality": 85}})
                                    assert repeated["result"] == rendered["result"], repeated
                                    scoped = dict(common, export=reduced_ticket, format=image_format, quality=85)
                                    mismatched = dict(scoped, generation=str(int(common["generation"]) + 1))
                                    assert client.tool("debug_render_export", mismatched)["error"]
                                    incomplete = dict(scoped)
                                    del incomplete["generation"]
                                    assert client.tool("debug_render_export", incomplete)["error"]
                                    valid_scope = client.rpc("tools/call", {"name": "debug_render_export", "arguments": scoped})
                                    assert valid_scope["result"] == rendered["result"], valid_scope
                                    foreign = other.rpc("tools/call", {"name": "debug_render_export",
                                        "arguments": {"connection": other_debug, "export": reduced_ticket,
                                                      "format": image_format, "quality": 85}})
                                    assert foreign["result"]["isError"], foreign
                                    with Image.open(BytesIO(encoded)) as picture:
                                        assert picture.size == (output_width, output_height)
                                        if image_format == "png":
                                            rgba = picture.convert("RGBA").tobytes()
                                            if source["format"] in (37, 43):
                                                assert rgba == actual
                                            elif source["format"] in (44, 50):
                                                assert rgba == bytes(value for offset in range(0, len(actual), 4)
                                                                     for value in (actual[offset + 2], actual[offset + 1],
                                                                                   actual[offset], actual[offset + 3]))
                                        elif source["format"] in (37, 43, 44, 50):
                                            rgb = picture.convert("RGB").tobytes()
                                            order = (0, 1, 2) if source["format"] in (37, 43) else (2, 1, 0)
                                            expected_rgb = bytes(actual[offset + channel] for offset in range(0, len(actual), 4)
                                                                 for channel in order)
                                            assert sum(abs(a - b) for a, b in zip(rgb, expected_rgb)) / len(rgb) < 50
                                sdk_ticket = observer.start_export(native_image, layer=0,
                                    region=(5, 7, 64, 48, output_width, output_height))["export_id"]

                                def sdk_region_ready():
                                    result = observer.export_info(sdk_ticket)
                                    assert result["phase"] != "failed", result
                                    return result if result["phase"] == "complete" else None

                                sdk_info = eventually(sdk_region_ready)
                                assert sdk_info["image_region"] == region
                                assert observer.read_export(sdk_ticket, 0, reduced_bytes)["bytes"] == actual
                                assert observer.release_export(sdk_ticket)["released"]
                                same = client.tool("debug_start_export", {"connection": debug, "image": region_image,
                                                   "layer": 0, "region": region})
                                assert not same["error"], same
                                same_ticket = same["data"]["export"]
                                def same_ready():
                                    result = client.tool("debug_export_info", {"connection": debug, "export": same_ticket})
                                    assert not result["error"] and result["data"]["phase"] != "failed", result
                                    return result["data"]["phase"] == "complete"
                                eventually(same_ready)
                                same_render = client.rpc("tools/call", {"name": "debug_render_export",
                                    "arguments": {"connection": debug, "export": same_ticket,
                                                  "format": "jpeg", "quality": 85}})
                                assert not same_render["result"]["isError"], same_render
                                assert same_render["result"]["content"][1] == content[1]
                                assert json.loads(same_render["result"]["content"][0]["text"])["export"] == same_ticket
                                assert not client.tool("debug_release_export", {"connection": debug, "export": same_ticket})["error"]
                                assert not client.tool("debug_release_export", {"connection": debug,
                                                       "export": reduced_ticket})["error"]
                                expired = client.rpc("tools/call", {"name": "debug_render_export",
                                    "arguments": {"connection": debug, "export": reduced_ticket,
                                                  "format": "png", "quality": 85}})
                                assert expired["result"]["isError"], expired
                            invalid_region = {"x": source["width"], "y": 0, "width": 1, "height": 1,
                                              "output_width": 1, "output_height": 1}
                            assert client.tool("debug_start_export", {"connection": debug, "image": region_image,
                                               "region": invalid_region})["error"]
                            for _ in range(10):
                                cycle = client.tool("debug_start_export", {"connection": debug, "image": region_image,
                                                    "layer": 0, "region": small_region})
                                assert not cycle["error"], cycle
                                cycle_ticket = cycle["data"]["export"]

                                def cycle_ready():
                                    status = client.tool("debug_export_info", {"connection": debug, "export": cycle_ticket})
                                    assert not status["error"] and status["data"]["phase"] != "failed", status
                                    return status["data"] if status["data"]["phase"] == "complete" else None

                                eventually(cycle_ready)
                                cycle_link = client.rpc("tools/call", {"name": "debug_export_artifact", "arguments":
                                                        {"connection": debug, "export": cycle_ticket}})
                                assert not cycle_link["result"]["isError"], cycle_link
                                assert not client.tool("debug_release_export", {"connection": debug,
                                                       "export": cycle_ticket})["error"]
                            assert not client.tool("debug_release_image", {"connection": debug,
                                                   "image": region_image})["error"]
                            assert observer.release_export(native_export)["released"]
                            assert observer.release_image(native_image)["released"]
                        assert not client.tool("debug_release_export", {"connection": debug, "export": ticket})["error"]
                        assert download(authorization)[0] == 404
                        assert client.tool("debug_export_info", {"connection": debug, "export": ticket})["error"]
                        checked_export = True
                    saw_lookahead = len(capture["captures"]) >= 2
                    for capture_id in dict.fromkeys(capture["captures"] + [capture["capture_id"]]):
                        released = client.tool("debug_release", {"connection": debug, "lease": lease,
                                               "command_id": str(next_command), "capture": capture_id})
                        assert not released["error"], released
                        eventually(lambda: operation(released["data"]["operation_id"]))
                        next_command += 1
                    if release_inspection:
                        feature = client.tool("debug_revision", {"connection": debug, "revision": revision})
                        assert not feature["error"] and feature["data"]["kind"] == "view_settings_v1", feature
                        released_inspection = client.tool("debug_release_inspection", release_inspection)
                        assert not released_inspection["error"] and released_inspection["data"]["released"]
                        assert client.tool("debug_revision", {"connection": debug, "revision": revision})["error"]
                        repeated_release = client.tool("debug_release_inspection", release_inspection)
                        assert not repeated_release["error"] and not repeated_release["data"]["released"]
                    if saw_lookahead:
                        break
                assert saw_lookahead, "Lookahead never exposed multiple retained captures"
                applied = client.tool("simulation_status", {"connection": debug, "transaction_id": transaction["transaction_id"]})
                assert not applied["error"] and applied["data"]["phase"] == "applied", applied
                history = client.tool("debug_frame_history", {"connection": debug, "after": str(frame - 1), "count": 1})
                assert not history["error"] and len(history["data"]["frames"]) == 1, history
                historic, = history["data"]["frames"]
                assert historic["frame_id"] == str(frame) and history["data"]["next_after"] == str(frame), history
                assert historic["prediction"]["frame_id"] == historic["frame_id"]
                historical_input = historic["applied_input"]
                historical_config = historical_input["configuration"]
                assert historical_input["context_available"] and "focused" not in historical_config
                assert abs(historical_config["head"]["position"][0] - 2) < 0.001
                assert abs(historical_config["controllers"][0]["controls"]["trigger"] - 0.75) < 0.001
                assert historical_config["body"]["preset"] == 2
                assert historical_config["trackers"][0]["id"] == "9007199254740993"
                assert historical_config["eye_views"]["space_revision"] == "9007199254740993"
                historical_commands = [decode_command(command) for command in historical_input["commands"]]
                assert next(command for command in historical_commands if command["kind"] == "trackers")["devices"][0]["identity"] == 9007199254740993
                assert len(historical_input["context"]) == 6
                assert client.tool("debug_frame_history", {"connection": debug, "count": 129})["error"]
                assert client.tool("debug_frame_history", {"connection": debug, "after": "18446744073709551616"})["error"]
                assert other.tool("debug_frame_history", {"connection": debug})["error"]
                observed = client.tool("simulation_status", {"connection": debug})
                assert not observed["error"], observed
                configuration = observed["data"]["document"]["configuration"]
                assert abs(configuration["head"]["position"][0] - 2) < 0.001
                assert abs(configuration["controllers"][0]["controls"]["trigger"] - 0.75) < 0.001
                assert configuration["body"]["preset"] == 2
                assert abs(configuration["body"]["controls"][0]["offset"][0] - 0.1) < 0.001
                assert configuration["body"]["joints"][0]["joint"] == 2 and not configuration["body"]["joints"][0]["tracked"]
                tracker, = configuration["trackers"]
                assert tracker["id"] == "9007199254740993" and tracker["anchor"] == 5
                assert tracker["source_path"] == "test:/left-foot" and abs(tracker["joint_offset"]["position"][0] - 0.2) < 0.001
                assert configuration["reference_spaces"]["local"]["position"][0] == 0.25
                assert configuration["reference_spaces"]["stage_bounds"] == [3, 4]
                eyes = configuration["eye_views"]
                assert eyes["space_revision"] == "9007199254740993"
                assert abs(eyes["eyes"][0]["pose"]["position"][0] + 0.03) < 0.001
                assert abs(eyes["eyes"][1]["fov"][1] - 0.8) < 0.001
                tracking = client.tool("get_head_pose", {"application_id": application_id})
                assert not tracking["error"] and abs(tracking["data"]["configured_head_pose"]["position"][0] - 2) < 0.001, tracking
                diagnostics = client.tool("get_diagnostics", {"application_id": application_id})
                assert not diagnostics["error"] and diagnostics["data"]["runtime_available"], diagnostics
                assert diagnostics["data"]["generation"] == state["generation"], diagnostics
                assert "events" in diagnostics["data"]["diagnostics"] and "logs_unavailable" in diagnostics["data"]
                events = diagnostics["data"]["diagnostics"]["events"]
                cursor = int(diagnostics["data"]["diagnostic_cursor_sample"])
                sequences = [int(event["sequence"]) for event in events]
                assert len(sequences) <= 64 and sequences == sorted(sequences)
                assert all(sequence > max(0, cursor - 65) for sequence in sequences), diagnostics
                issue = client.tool("diagnose_issue", {"application_id": application_id, "symptoms": "head tracking"})
                assert not issue["error"] and issue["data"]["symptoms"] == "head tracking" and issue["data"]["observations"], issue
                state_info = client.tool("get_session_state", {"application_id": application_id})
                assert not state_info["error"] and state_info["data"]["session_state"] in ("visible", "focused", "synchronized"), state_info

                def observed_views():
                    probe_state = json.loads((root / "probe_state.json").read_text())
                    return probe_state if abs(probe_state["viewSpan"] - 0.06) < 0.001 and \
                        abs(probe_state["leftFovRight"] - 0.9) < 0.001 else None

                eventually(observed_views)
                assert not client.tool("simulation_stop_recording", {"connection": debug, "recording_id": recording_id})["error"]
                recording = client.tool("simulation_read_recording", {"connection": debug, "recording_id": recording_id})
                assert not recording["error"] and recording["data"]["document"]["total"] == 2, recording
                records = recording["data"]["document"]["transactions"]
                assert {record["phase"] for record in records} == {"applied", "cancelled"}, records
                assert all(record["transaction_id"] == transaction["transaction_id"] for record in records)
                assert len({record["source"]["id"] for record in records}) == 2, records
                assert client.tool("simulation_read_recording", {"connection": debug})["error"]
                assert client.tool("simulation_read_recording", {"connection": debug, "recording_id": "999999"})["error"]
                assert other.tool("simulation_acknowledge_recording", {"connection": other_debug,
                                  "recording_id": recording_id, "through": "2"})["error"]
                ack = client.tool("simulation_acknowledge_recording", {"connection": debug,
                                  "recording_id": recording_id, "through": "2"})
                assert not ack["error"] and ack["data"]["document"]["floor"] == 2, ack
                assert client.tool("simulation_read_recording", {"connection": debug,
                                   "recording_id": recording_id, "offset": "0"})["error"]
                canonical = next(record for record in records if record["phase"] == "applied")["commands"]
                assert len(canonical) == 12 and all(item["encoding"] == "simxr-command-v1-hex" for item in canonical)
                invalid_canonical = {"connection": debug, "transaction_id": "9007199254741001", "frame": "5000",
                                     "commands": [{"encoding": "simxr-command-v1-hex", "data": "ff0"}]}
                assert client.tool("simulation_schedule", invalid_canonical)["error"]
                invalid_canonical["commands"][0]["data"] = "00" * 131073
                assert client.tool("simulation_schedule", invalid_canonical)["error"]
                replay = client.tool("simulation_schedule", {"connection": debug, "transaction_id": "9007199254741001",
                                     "frame": "5000", "commands": canonical})
                assert not replay["error"] and replay["data"]["phase"] == "queued", replay
                cancelled = client.tool("simulation_cancel", {"connection": debug, "transaction_id": "9007199254741001"})
                assert not cancelled["error"] and cancelled["data"]["phase"] == "cancelled", cancelled
                ownership = other.tool("simulation_schedule", {"connection": other_debug,
                                       "transaction_id": "9007199254741000",
                                       "frame": str(int(observed["data"]["document"]["frame_id"]) + 1),
                                       "commands": [{"kind": "take_control", "channel": "gaze"}]})
                assert not ownership["error"] and ownership["data"]["phase"] == "queued", ownership
                for _ in range(8):
                    step = client.tool("debug_step", {"connection": debug, "lease": lease,
                                       "command_id": str(next_command), "lookahead": 1})
                    assert not step["error"], step
                    next_command += 1
                    capture = eventually(lambda: operation(step["data"]["operation_id"]))
                    for capture_id in dict.fromkeys(capture["captures"] + [capture["capture_id"]]):
                        released = client.tool("debug_release", {"connection": debug, "lease": lease,
                                               "command_id": str(next_command), "capture": capture_id})
                        assert not released["error"], released
                        eventually(lambda: operation(released["data"]["operation_id"]))
                        next_command += 1
                    owned = other.tool("simulation_status", {"connection": other_debug,
                                       "transaction_id": "9007199254741000"})
                    if not owned["error"] and owned["data"]["phase"] != "queued":
                        break
                assert not owned["error"] and owned["data"]["phase"] == "applied", owned
                assert not client.tool("simulation_release_control", {"connection": debug})["error"]
                resumed = client.tool("debug_resume", {"connection": debug, "lease": lease,
                                      "command_id": str(next_command)})
                assert not resumed["error"], resumed
                eventually(lambda: operation(resumed["data"]["operation_id"]))
                assert not client.tool("debug_acknowledge", {"connection": debug, "lease": lease,
                                       "through": str(next_command)})["error"]
                assert not client.tool("debug_relinquish", {"connection": debug, "lease": lease})["error"]

                def released_views():
                    status = client.tool("simulation_status", {"connection": debug})
                    assert not status["error"], status
                    config = status["data"]["document"]["configuration"]
                    return config if config["eye_views"] is None else None

                released_config = eventually(released_views)
                assert released_config["reference_spaces"]["local"]["position"][0] == 0.25
                assert released_config["reference_spaces"]["stage_bounds"] == [3, 4]
                other_lease = other.tool("debug_acquire", {"connection": other_debug})
                assert not other_lease["error"], other_lease
                if expect_luau:
                    script = client.tool("simulator_luau_run", {"application_id": application_id,
                                         "source": """print('native MCP rig')
local frame = simxr.frame_add(simxr.state().frame_id, '1')
local transaction = simxr.schedule({frame=frame, late_policy='reschedule_next', commands={
    {kind='take_control', channel='head'}, {kind='head_pose', position={3, 1.7, 0}}
}})
simxr.await_transaction(transaction)
simxr.checkpoint('ran', simxr.state().configuration.head.position[1] == 3)
simxr.release_inputs()
""",
                                         "seed": "9007199254741009"})
                    assert not script["error"] and script["data"]["accepted"], script
                    run = script["data"]["run_id"]

                    def script_status(selected, identity):
                        status = selected.tool("simulator_luau_status", {"application_id": application_id, "run_id": identity})
                        assert not status["error"], status
                        return status["data"] if status["data"]["phase"] not in ("running", "paused") else None

                    completed = eventually(lambda: script_status(client, run))
                    assert completed["phase"] == "complete" and completed["seed"] == "9007199254741009", completed
                    assert completed["output"] == ["native MCP rig"] and completed["checkpoints"][0]["passed"], completed
                    assert completed["transactions"] == "1", completed
                    current = client.tool("simulator_luau_status", {"application_id": application_id})
                    assert not current["error"] and current["data"]["run_id"] == run, current
                    assert other.tool("simulator_luau_status", {"application_id": application_id, "run_id": run})["error"]
                    assert other.tool("simulator_luau_cancel", {"application_id": application_id, "run_id": run})["error"]
                    held_script = other.tool("simulator_luau_run", {"application_id": application_id,
                                             "source": "while true do end", "deadline_seconds": 30})
                    assert not held_script["error"], held_script
                    assert client.tool("simulator_luau_run", {"application_id": application_id,
                                       "source": "print('must not start')"})["error"]
                other_bridge.stdin.close()
                assert other_bridge.wait(timeout=15) == 0
                assert download(authorization, other_url)[0] == 404

                if expect_luau:
                    def run_after_disconnect():
                        result = client.tool("simulator_luau_run", {"application_id": application_id,
                                             "source": "while true do simxr.state() end", "deadline_seconds": 30})
                        if result["error"]:
                            assert "current rig" in result["data"]["error"], result
                            return None
                        return result["data"]["run_id"]

                    cancellable = eventually(run_after_disconnect)
                    assert client.tool("simulator_luau_status", {"application_id": application_id, "run_id": run})["error"]
                    paused_luau = client.tool("simulator_luau_pause", {"application_id": application_id, "run_id": cancellable})
                    assert not paused_luau["error"] and paused_luau["data"]["accepted"], paused_luau

                    def script_paused():
                        status = client.tool("simulator_luau_status", {"application_id": application_id, "run_id": cancellable})
                        assert not status["error"], status
                        assert status["data"]["phase"] in ("running", "paused"), status
                        return status["data"]["phase"] == "paused"

                    eventually(script_paused)
                    assert not client.tool("simulator_luau_step", {"application_id": application_id,
                                           "run_id": cancellable})["error"]
                    assert not client.tool("simulator_luau_resume", {"application_id": application_id,
                                           "run_id": cancellable})["error"]
                    cancelled = client.tool("simulator_luau_cancel", {"application_id": application_id, "run_id": cancellable})
                    assert not cancelled["error"] and cancelled["data"]["accepted"], cancelled
                    assert eventually(lambda: script_status(client, cancellable))["phase"] == "cancelled"

                observed = client.tool("simulation_status", {"connection": debug})
                transfer = client.tool("simulation_schedule", {"connection": debug,
                                       "transaction_id": "9007199254741002",
                                       "frame": str(int(observed["data"]["document"]["frame_id"]) + 1),
                                       "late_policy": "reschedule_next",
                                       "commands": [{"kind": "take_control", "channel": "gaze"}]})
                assert not transfer["error"] and transfer["data"]["phase"] in ("queued", "rescheduled"), transfer

                def transferred():
                    status = client.tool("simulation_status", {"connection": debug,
                                         "transaction_id": "9007199254741002"})
                    assert not status["error"], status
                    return status["data"] if status["data"]["phase"] not in ("queued", "rescheduled") else None

                transfer_result = eventually(transferred)
                assert transfer_result["phase"] == "applied", transfer_result

                def acquire_after_disconnect():
                    value = client.tool("debug_acquire", {"connection": debug})
                    return value if not value["error"] else None

                fresh = eventually(acquire_after_disconnect)
                assert not client.tool("debug_relinquish", {"connection": debug, "lease": fresh["data"]["lease"]})["error"]
                if expect_luau:
                    held = client.tool("simulator_luau_run", {"application_id": application_id,
                                       "source": """local frame = simxr.frame_add(simxr.state().frame_id, '1')
local transaction = simxr.schedule({frame=frame, late_policy='reschedule_next', commands={
    {kind='take_control', channel='head'}, {kind='head_pose', position={4, 1.7, 0}}
}})
simxr.await_transaction(transaction)
simxr.start_input_recording()
simxr.pause()
print('held resources')
while true do end
""", "deadline_seconds": 30})
                    assert not held["error"], held
                    held_run = held["data"]["run_id"]

                    def resources_held():
                        status = client.tool("simulator_luau_status", {"application_id": application_id, "run_id": held_run})
                        assert not status["error"] and status["data"]["phase"] == "running", status
                        return "held resources" in status["data"]["output"]

                    eventually(resources_held)
                    assert client.tool("debug_acquire", {"connection": debug})["error"]
                    assert client.tool("simulation_start_recording", {"connection": debug})["error"]
                    assert not client.tool("simulator_luau_cancel", {"application_id": application_id, "run_id": held_run})["error"]
                    assert eventually(lambda: script_status(client, held_run))["phase"] == "cancelled"
                    started_recording = client.tool("simulation_start_recording", {"connection": debug, "drain": True})
                    assert not started_recording["error"], started_recording
                    recording_id = started_recording["data"]["document"]["recording_id"]
                    assert not client.tool("simulation_stop_recording", {"connection": debug, "recording_id": recording_id})["error"]
                    fresh = eventually(acquire_after_disconnect)
                    assert not client.tool("debug_relinquish", {"connection": debug, "lease": fresh["data"]["lease"]})["error"]
                    observed = client.tool("simulation_status", {"connection": debug})
                    takeover = client.tool("simulation_schedule", {"connection": debug,
                                           "transaction_id": "9007199254741003",
                                           "frame": str(int(observed["data"]["document"]["frame_id"]) + 1),
                                           "late_policy": "reschedule_next", "commands": [
                                               {"kind": "take_control", "channel": "head"},
                                               {"kind": "head_pose", "position": [5, 1.7, 0]}]})
                    assert not takeover["error"], takeover

                    def takeover_result():
                        status = client.tool("simulation_status", {"connection": debug,
                                             "transaction_id": "9007199254741003"})
                        assert not status["error"], status
                        return status["data"] if status["data"]["phase"] not in ("queued", "rescheduled") else None

                    assert eventually(takeover_result)["phase"] == "applied"
                    assert not client.tool("simulation_release_control", {"connection": debug})["error"]
                observed = client.tool("simulation_status", {"connection": debug})
                assert not observed["error"], observed
                started_recording = client.tool("simulation_start_recording", {"connection": debug, "drain": True})
                assert not started_recording["error"], started_recording
                recording_id = started_recording["data"]["document"]["recording_id"]
                provider_joints = [{"joint": joint, "position": [0, 1, 0], "orientation": [0, 0, 0, 1], "flags": 15}
                                   for joint in sorted(JOINT_SETS[2])]
                provider = {"joint_set": 2, "requested_time": "9007199254740993", "located": True,
                            "all_joint_poses_tracked": True, "joints": provider_joints}
                body_tx = client.tool("simulation_schedule", {"connection": debug,
                                      "transaction_id": "9007199254741004",
                                      "frame": str(int(observed["data"]["document"]["frame_id"]) + 1),
                                      "late_policy": "reschedule_next", "commands": [
                                          {"kind": "take_control", "channel": "body"},
                                          {"kind": "body", "observation": provider}]})
                assert not body_tx["error"], body_tx

                def body_result():
                    status = client.tool("simulation_status", {"connection": debug,
                                         "transaction_id": "9007199254741004"})
                    assert not status["error"], status
                    return status["data"] if status["data"]["phase"] not in ("queued", "rescheduled") else None

                assert eventually(body_result)["phase"] == "applied"
                assert not client.tool("simulation_stop_recording", {"connection": debug, "recording_id": recording_id})["error"]
                recording = client.tool("simulation_read_recording", {"connection": debug, "recording_id": recording_id})
                assert not recording["error"], recording
                records = recording["data"]["document"]["transactions"]
                assert len(records) == 1 and records[0]["phase"] == "applied", records
                recorded_body = decode_command(records[0]["commands"][1])["observation"]
                assert recorded_body["requested_time"] == 9007199254740993
                assert {joint["joint"] for joint in recorded_body["joints"]} == JOINT_SETS[2]
                assert not client.tool("simulation_release_control", {"connection": debug})["error"]
                bad = client.tool("simulator_restart_application", {"application_id": application_id,
                                  "process_id": str(child_pid + 1)})
                assert bad["error"]
                restarted = client.tool("simulator_restart_application", {"application_id": application_id,
                                        "process_id": str(child_pid)})
                assert not restarted["error"] and restarted["data"]["accepted"]

                def reconnected():
                    state = connected()
                    return state if state and int(state["process_id"]) != child_pid else None

                replacement = eventually(reconnected, timeout=30)
                old_pid = child_pid
                child_pid = int(replacement["process_id"])
                assert not replacement["restart_pending"]
                assert state["endpoint"] == str(root / f"control.{old_pid}.sock")
                assert replacement["endpoint"] == str(root / f"control.{child_pid}.sock")
                assert client.tool("debug_status", {"connection": debug})["error"]
                stale = client.tool("simulator_restart_application", {"application_id": application_id,
                                    "process_id": str(old_pid)})
                assert stale["error"]
                listed = client.tool("simulator_list_applications", {})
                assert any(item["application_id"] == application_id for item in listed["data"]["applications"])
                short_ids = []
                for _ in range(65):
                    short = client.tool("simulator_launch_application", {"executable": "/bin/true"})
                    assert not short["error"], short
                    short_id = short["data"]["application_id"]
                    short_ids.append(short_id)

                    def exited():
                        status = client.tool("simulator_application_status", {"application_id": short_id})
                        assert not status["error"], status
                        assert status["data"]["launch_state"] != "failed", status
                        return status["data"]["launch_state"] == "exited" and status["data"]["exit_code"] == 0

                    eventually(exited)
                assert len(set(short_ids)) == 65
                assert client.tool("simulator_application_status", {"application_id": short_ids[0]})["error"]
                unavailable = client.tool("get_diagnostics", {"application_id": short_ids[-1]})
                assert not unavailable["error"] and not unavailable["data"]["runtime_available"], unavailable
                absent = client.tool("get_session_state", {"application_id": short_ids[-1]})
                assert absent["error"] and not absent["data"]["ok"], absent
                assert not client.tool("simulator_application_status", {"application_id": application_id})["error"]
                shutdown = client.tool("simulator_shutdown", {})
                assert not shutdown["error"] and shutdown["data"]["accepted"]
                eventually(lambda: not Path(f"/proc/{app_pid}/cmdline").exists() or
                           not Path(f"/proc/{app_pid}/cmdline").read_bytes(), timeout=15)
                os.kill(child_pid, 0)
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                print("PASS windowless connector startup, real XR session, MCP shutdown and game survival")
            finally:
                if other_bridge and other_bridge.poll() is None:
                    other_bridge.kill()
                    other_bridge.wait()
                if bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                for pid in (app_pid, child_pid):
                    if pid:
                        try:
                            os.kill(pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass


if __name__ == "__main__":
    main()
