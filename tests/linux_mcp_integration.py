#!/usr/bin/env python3
"""Exercise real MCP stdio tools against the running loader/Vulkan probe."""
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

async def test(data):
    env = dict(os.environ, XR_RUNTIME_JSON=str(ROOT / 'bin/openxr_simulator.json'),
               SIMXR_DATA_DIR=str(data), SIMXR_PROBE_FRAMES='300', SIMXR_PROBE_INTERACTIVE='1',
               SIMXR_DETERMINISTIC='0')
    with (data / 'probe-output.log').open('w') as log:
        probe = subprocess.Popen([str(ROOT / 'bin/xr_probe_linux')], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            await wait_state(data / 'probe_state.json', lambda s: s['frame'] >= 4)
            server = StdioServerParameters(command=sys.executable,
                args=[str(ROOT / 'mcp-server/openxr_simulator_mcp.py')], env=env)
            async with stdio_client(server) as (reader, writer):
                async with ClientSession(reader, writer) as session:
                    await session.initialize()
                    names = {tool.name for tool in (await session.list_tools()).tools}
                    assert {'run_pose_sequence', 'set_controller_pose', 'hold_controller_inputs'} <= names
                    assert 'enable_wheel_sweep' not in names
                    result = await session.call_tool('set_head_pose', {'x': .8, 'y': 1.8, 'yaw_deg': 30})
                    assert not result.isError
                    result = await session.call_tool('hold_controller_inputs', {'hand': 1, 'trigger': .4})
                    assert not result.isError
                    result = await session.call_tool('set_controller_pose', {'hand': 1, 'posX': .35, 'posY': -.3, 'posZ': -.4})
                    assert not result.isError
                    observed = await wait_state(data / 'probe_state.json',
                        lambda s: abs(s['headX']-.8)<.0001 and abs(s['trigger']-.4)<.0001 and abs(s['handX']-.35)<.0001)
                    result = await session.call_tool('capture_screenshot', {'eye': 'both', 'layer': 'all', 'timeout': 3})
                    assert not result.isError and any(item.type == 'image' for item in result.content)
                    image = Image.open(BytesIO(base64.b64decode(next(item.data for item in result.content if item.type=='image')))).convert('RGB')
                    assert image.size==(128,48)
                    assert image.getpixel((96,24))[0]>230 and image.getpixel((96,24))[2]<20
                    assert image.getpixel((124,4))[2]>230 and image.getpixel((124,4))[0]<20
                    result = await session.call_tool('capture_screenshot', {'eye': 'right', 'layer': 'projection', 'timeout': 3})
                    assert not result.isError
                    image = Image.open(BytesIO(base64.b64decode(next(item.data for item in result.content if item.type=='image')))).convert('RGB')
                    assert image.size==(64,48) and image.getpixel((32,24))[2]>230
                    result = await session.call_tool('capture_screenshot', {'eye': 'both', 'layer': 'quad', 'timeout': 3})
                    assert not result.isError
                    image = Image.open(BytesIO(base64.b64decode(next(item.data for item in result.content if item.type=='image')))).convert('RGB')
                    assert image.size==(128,48)
                    assert max(image.getpixel((32,24)))<20 and image.getpixel((96,24))[0]>230
                    result = await session.call_tool('get_projection_log', {'max_entries': 2})
                    assert not result.isError
                    entries = json.loads(result.content[0].text)['entries']
                    assert len(entries)==2 and entries[-1]['right_rect']==[0,0,64,48]
                    result = await session.call_tool('get_frame_info', {})
                    assert not result.isError
                    print('PASS: live MCP pose/controller/input commands reached OpenXR queries, capture returned image, projection log matched submitted rects; frame', observed['frame'])
            # The probe must still pass Vulkan/lifecycle checks after external control.
            code = await asyncio.to_thread(probe.wait, timeout=10)
            assert code==0, (data/'probe-output.log').read_text()
        finally:
            if probe.poll() is None:
                probe.terminate()
                probe.wait(timeout=5)

if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='simxr-live-mcp-') as directory:
        asyncio.run(test(Path(directory)))
