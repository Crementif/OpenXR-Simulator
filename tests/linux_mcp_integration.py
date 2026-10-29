#!/usr/bin/env python3
"""Exercise real MCP stdio tools against the running loader/Vulkan probe."""
import argparse
import asyncio
import base64
from io import BytesIO
from PIL import Image
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]

async def wait_state(path, predicate, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = json.loads(path.read_text())
            if predicate(value):
                return value
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        await asyncio.sleep(.02)
    raise AssertionError(f"State did not arrive: {path}")


def retained_image(result, transport, layer, eye):
    assert not result.isError and any(item.type == 'image' for item in result.content), result
    metadata = json.loads(next(item.text for item in result.content if item.type == 'text').splitlines()[-1])
    assert metadata['transport'] == transport and metadata['layer'] == layer and metadata['eye'] == eye, metadata
    assert int(metadata['frame']['frame_id']) > 0 and int(metadata['frame']['generation']) > 0, metadata
    image = Image.open(BytesIO(base64.b64decode(next(item.data for item in result.content if item.type == 'image')))).convert('RGB')
    assert image.width > 0 and image.height > 0 and max(image.size) <= 1280, image.size
    assert abs(image.width / image.height - metadata['width'] / metadata['height']) < .01, (image.size, metadata)
    return image


async def test(data, binaries):
    env = dict(os.environ, XR_RUNTIME_JSON=str(binaries / 'openxr_simulator.json'),
               SIMXR_DATA_DIR=str(data), SIMXR_PROBE_FRAMES='300', SIMXR_PROBE_INTERACTIVE='1',
               SIMXR_DETERMINISTIC='0')
    with (data / 'probe-output.log').open('w') as log:
        probe = subprocess.Popen([str(binaries / 'xr_probe_linux')], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            await wait_state(data / 'probe_state.json', lambda s: s['frame'] >= 4)
            server = StdioServerParameters(command=sys.executable,
                args=[str(ROOT / 'mcp-server/openxr_simulator_mcp.py')], env=env)
            async with stdio_client(server) as (reader, writer):
                async with ClientSession(reader, writer) as session:
                    await session.initialize()
                    names = {tool.name for tool in (await session.list_tools()).tools}
                    assert {'run_pose_sequence', 'set_controller_pose', 'hold_controller_inputs'} <= names
                    result = await session.call_tool('set_head_pose', {'x': .8, 'y': 1.8, 'yaw_deg': 30})
                    assert not result.isError and json.loads(result.content[0].text)['ok'], result
                    await wait_state(data / 'probe_state.json', lambda s: abs(s['headX']-.8)<.0001)
                    result = await session.call_tool('hold_controller_inputs', {'hand': 1, 'trigger': .4})
                    assert not result.isError and json.loads(result.content[0].text)['ok'], result
                    await wait_state(data / 'probe_state.json', lambda s: abs(s['trigger']-.4)<.0001)
                    result = await session.call_tool('set_controller_pose', {'hand': 1, 'posX': .35, 'posY': -.3, 'posZ': -.4})
                    assert not result.isError and json.loads(result.content[0].text)['ok'], result
                    observed = await wait_state(data / 'probe_state.json',
                        lambda s: abs(s['headX']-.8)<.0001 and abs(s['trigger']-.4)<.0001 and abs(s['handX']-.35)<.0001)
                    result = await session.call_tool('capture_screenshot', {'eye': 'both', 'layer': 'all', 'timeout': 3})
                    image = retained_image(result, 'retained_preview', 'all', 'both')
                    center = image.getpixel((image.width * 3 // 4, image.height // 2))
                    corner = image.getpixel((image.width * 97 // 100, image.height * 8 // 100))
                    assert center[0] > 230 and center[2] < 20 and corner[2] > 230 and corner[0] < 20
                    result = await session.call_tool('capture_screenshot', {'eye': 'right', 'layer': 'projection', 'timeout': 3})
                    image = retained_image(result, 'retained_capture', 'projection', 'right')
                    assert image.getpixel((image.width // 2, image.height // 2))[2] > 230
                    result = await session.call_tool('capture_screenshot', {'eye': 'both', 'layer': 'quad', 'timeout': 3})
                    image = retained_image(result, 'retained_capture', 'quad', 'both')
                    assert max(image.getpixel((image.width // 4, image.height // 2))) < 20
                    assert image.getpixel((image.width * 3 // 4, image.height // 2))[0] > 230
                    result = await session.call_tool('get_projection_log', {'max_entries': 2})
                    assert not result.isError
                    entries = json.loads(result.content[0].text)['entries']
                    assert len(entries)==2 and entries[-1]['right_rect']==[0,0,64,48]
                    result = await session.call_tool('get_frame_info', {})
                    assert not result.isError
                    print('PASS: live MCP pose/controller/input commands reached OpenXR queries, capture returned image, projection log matched submitted rects; frame', observed['frame'])
            code = await asyncio.to_thread(probe.wait, timeout=10)
            assert code==0, (data/'probe-output.log').read_text()
        finally:
            if probe.poll() is None:
                probe.terminate()
                probe.wait(timeout=5)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path, help='CMake build directory with SIMXR_BUILD_PROBES=ON')
    binaries = parser.parse_args().build.resolve() / 'bin'
    for name in ('openxr_simulator.json', 'xr_probe_linux'):
        if not (binaries / name).is_file():
            parser.error(f'Missing {binaries / name}; build the runtime and probes first')
    with tempfile.TemporaryDirectory(prefix='simxr-live-mcp-') as directory:
        asyncio.run(test(Path(directory), binaries))
