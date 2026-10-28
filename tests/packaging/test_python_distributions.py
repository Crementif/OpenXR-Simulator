#!/usr/bin/env python3
"""Exercise installed Python distributions from outside their source checkout."""

import asyncio
import argparse
from importlib import metadata
import json
import os
from pathlib import Path
import sys
import sysconfig
import tempfile
import subprocess

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
import debug_control
import simxr_control


def require_installed(module):
    location = Path(module.__file__).resolve()
    if not location.is_relative_to(Path(sys.prefix).resolve()):
        raise AssertionError(f'{module.__name__} was not imported from the test environment: {location}')


async def exercise(probe):
    require_installed(simxr_control)
    require_installed(debug_control)
    assert metadata.version('simxr-control').split('.')[0] == '2'
    assert metadata.version('openxr-simulator-mcp').split('.')[0] == '2'
    assert any(requirement.startswith('simxr-control') for requirement in metadata.requires('openxr-simulator-mcp'))
    script = Path(sysconfig.get_path('scripts')) / ('openxr-simulator-mcp.exe' if os.name == 'nt' else 'openxr-simulator-mcp')
    assert script.is_file(), script
    with tempfile.TemporaryDirectory(prefix='simxr-python-installed-') as directory:
        env = dict(os.environ, SIMXR_DATA_DIR=directory)
        env.pop('PYTHONPATH', None)
        env.pop('SIMXR_CONTROL_SOCKET', None)
        child = None
        try:
            if probe:
                probe_env = dict(env, SIMXR_PROBE_CONTROL='1')
                child = subprocess.Popen([str(probe)], env=probe_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                ready = Path(directory) / 'control.ready'
                while not ready.exists():
                    if child.poll() is not None:
                        raise AssertionError(f'Probe exited early: {child.communicate()[0]!r}')
                    await asyncio.sleep(0.05)
            args = StdioServerParameters(command=str(script), env=env, cwd=directory)
            async with stdio_client(args) as (reader, writer):
                async with ClientSession(reader, writer) as client:
                    await client.initialize()
                    names = {tool.name for tool in (await client.list_tools()).tools}
                    assert {'debug_connect', 'capture_screenshot', 'simulation_schedule'} <= names
                    result = await client.call_tool('debug_connect', {'path': str(Path(directory) / 'missing.sock')})
                    assert not result.isError, result
                    payload = json.loads(result.content[0].text)
                    assert not payload['ok'] and payload['error'] != 'unavailable', payload
                    if child:
                        result = await client.call_tool('debug_connect', {'path': str(Path(directory) / f'control.{child.pid}.sock')})
                        payload = json.loads(result.content[0].text)
                        assert not result.isError and payload['ok'], payload
                        session, = payload['sessions']
                        identity = payload['connection']
                        result = await client.call_tool('debug_status', {'connection': identity, **session})
                        payload = json.loads(result.content[0].text)
                        assert not result.isError and payload['ok'], payload
                        result = await client.call_tool('debug_disconnect', {'connection': identity})
                        assert json.loads(result.content[0].text)['ok'], result
        finally:
            if child:
                (Path(directory) / 'control.continue').touch()
                try:
                    output, _ = await asyncio.to_thread(child.communicate, timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    output, _ = await asyncio.to_thread(child.communicate)
                    raise AssertionError(f'Probe did not exit: {output!r}')
                assert child.returncode == 0, output.decode(errors='replace')
    print('PASS installed SDK and MCP wheels, console entry point, tool discovery and SDK-backed error')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, help='Linux XR probe to connect to using XR_RUNTIME_JSON')
    options = parser.parse_args()
    asyncio.run(asyncio.wait_for(exercise(options.probe), timeout=30))
