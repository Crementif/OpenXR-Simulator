"""Actual MCP stdio -> Python SDK -> production OpenXR runtime control."""
import asyncio
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

try:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError:
    print('SKIP: MCP SDK is not installed in the selected test interpreter')
    raise SystemExit(77)

ROOT = Path(__file__).resolve().parents[2]


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


async def run():
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-native-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           PYTHONPATH=str(ROOT / 'sdk/python'))
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        probe = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 10
            while not (data / 'control.ready').exists():
                if probe.poll() is not None:
                    raise RuntimeError(probe.communicate()[0])
                require(time.monotonic() < deadline, 'Native control fixture did not start')
                await asyncio.sleep(0.01)
            options = StdioServerParameters(command=sys.executable,
                args=[str(ROOT / 'mcp-server/openxr_simulator_mcp.py')], env=environment)
            async with stdio_client(options) as (reader, writer):
                async with ClientSession(reader, writer) as mcp:
                    await mcp.initialize()
                    tools = {tool.name: tool for tool in (await mcp.list_tools()).tools}
                    require('debug_step' in tools and 'capture_screenshot' in tools, 'Tool registration lost compatibility')
                    require('command_id' in tools['debug_step'].inputSchema['required'], 'MCP commands must be idempotently identified')

                    async def call(name, arguments):
                        result = await mcp.call_tool(name, arguments)
                        require(not result.isError, result)
                        payload = json.loads(next(item.text for item in result.content if item.type == 'text'))
                        require(payload.get('ok'), payload)
                        return payload

                    connected = await call('debug_connect', {'path': str(data / f'control.{probe.pid}.sock')})
                    session, = connected['sessions']
                    require(isinstance(session['session'], str) and isinstance(session['generation'], str),
                            'MCP identifiers lost exact decimal-string representation')
                    common = {'connection': connected['connection'], **session}
                    status = await call('debug_status', common)
                    if not int(status['capabilities']) & (1 << 3):
                        (data / 'control.continue').touch()
                        output = (await asyncio.to_thread(probe.communicate, timeout=10))[0]
                        require(probe.returncode == 0, output)
                        print('SKIP: native owned capture unavailable')
                        return 77
                    lease = (await call('debug_acquire', common))['lease']
                    next_command = 1

                    async def command(name, **extra):
                        nonlocal next_command
                        arguments = {**common, 'lease': lease, 'command_id': str(next_command), **extra}
                        next_command += 1
                        return await call(name, arguments), arguments

                    async def operation(identity, *, settled=True):
                        deadline = time.monotonic() + 5
                        while True:
                            record = await call('debug_operation', {**common, 'operation': identity})
                            if record['phase'] != 'queued' and (not settled or record['phase'] != 'pending'):
                                return record
                            require(time.monotonic() < deadline, record)
                            await asyncio.sleep(0.01)

                    pause, _ = await command('debug_pause')
                    require((await operation(pause['operation_id']))['progress'] == 'paused', 'MCP pause did not settle')
                    await call('simulation_start_recording', common)
                    scheduled = await call('simulation_schedule', {**common, 'transaction_id': '1', 'frame': '3',
                        'commands': [{'kind': 'take_control', 'channel': 'gaze'}, {'kind': 'gaze', 'yaw': 0.25}]})
                    require(scheduled['phase'] == 'queued', scheduled)
                    retained = []
                    while True:
                        step, arguments = await command('debug_step')
                        duplicate = await call('debug_step', arguments)
                        require(duplicate['duplicate'] and duplicate['operation_id'] == step['operation_id'], 'MCP retry advanced twice')
                        record = await operation(step['operation_id'], settled=False)
                        if record['phase'] == 'pending':
                            break
                        require(record['progress'] == 'clear', record)
                        retained.append(record['capture_id'])
                    (data / 'control.continue').touch()
                    ready = await operation(step['operation_id'])
                    require(ready['progress'] == 'snapshot_ready' and ready['frame']['frame_id'] == '3', ready)
                    snapshot = await call('debug_capture', {**common, 'capture': ready['capture_id']})
                    require(snapshot['phase'] == 'ready' and snapshot['metadata_only'], snapshot)
                    require(snapshot['image_available'] and 'bytes' not in snapshot, snapshot)
                    inspected = await call('debug_inspect', {**common, 'capture': ready['capture_id']})
                    require(inspected['document']['frame_id'] == '3' and inspected['metadata_only'], inspected)
                    require(len(inspected['document']['layers']['layers']) == 2, inspected)
                    revision = await call('debug_revision', {'connection': common['connection'], 'revision': inspected['document']['view_settings']['revision_id']})
                    require(revision['revision_kind'] == 'view_settings_v1', revision)
                    released_reader = await call('debug_release_inspection', {'connection': common['connection'], 'inspection': inspected['inspection_id']})
                    require(released_reader['released'], released_reader)
                    applied = await call('simulation_status', {**common, 'transaction_id': '1'})
                    require(applied['phase'] == 'applied' and applied['document']['applied_frame'] == '3', applied)
                    await call('simulation_stop_recording', common)
                    recording = await call('simulation_read_recording', common)
                    require(recording['document']['total'] == 1 and recording['end'], recording)
                    diagnostics = await call('simulation_diagnostics', common)
                    require(any(event['kind'] == 'commands_applied' for event in diagnostics['document']['events']), diagnostics)
                    await call('simulation_release_control', common)
                    retained.append(ready['capture_id'])
                    displayed, _ = await command('debug_display_step')
                    display = await operation(displayed['operation_id'])
                    require(display['progress'] == 'displayed', display)
                    retained.append(display['capture_id'])
                    # Virtual-display ticks remain independent of producer/capture readiness.
                    # Earlier empty frames must remain explicit clears until frame3 is eligible.
                    for _ in range(8):
                        preview = await call('debug_retain_preview', common)
                        if preview['image_available']:
                            break
                        require(preview['display']['kind'] == 'clear' and 'image_reader_id' not in preview, preview)
                        displayed, _ = await command('debug_display_step')
                        display = await operation(displayed['operation_id'])
                        require(display['progress'] == 'displayed', display)
                        retained.append(display['capture_id'])
                    require(preview['image_available'] and preview['frame']['frame_id'] == '3' and
                            preview['capture_id'] == '0' and preview['target']['capture_kind'] == 3, preview)
                    await call('debug_release_image', {'connection': common['connection'], 'image': preview['image_reader_id']})
                    for target in ('layer_view', 'composed_eyes'):
                        step, _ = await command('debug_step', target=target, layer=0, view=0)
                        result = await operation(step['operation_id'])
                        require(result['progress'] == 'snapshot_ready' and result['frame']['frame_id'] == '4', result)
                        retained.append(result['capture_id'])
                        if target == 'layer_view':
                            image = await call('debug_retain_image', {**common, 'capture': result['capture_id']})
                            require(isinstance(image['image_reader_id'], str), image)
                            image_arguments = {'connection': common['connection'], 'image': image['image_reader_id']}
                            description = await call('debug_image_info', image_arguments)
                            require(description['image_descriptor']['array_layers'] == 1 and 'bytes' not in description,
                                    description)
                            exported = await call('debug_start_export', {**image_arguments, 'layer': 0})
                            export_arguments = {'connection': common['connection'], 'export': exported['export_id']}
                            require((await call('debug_release_image', image_arguments))['released'], image)
                            deadline = time.monotonic() + 5
                            while True:
                                export_status = await call('debug_export_info', export_arguments)
                                if export_status['phase'] == 'complete':
                                    break
                                require(export_status['phase'] == 'pending' and time.monotonic() < deadline,
                                        export_status)
                                await asyncio.sleep(0.01)
                            chunk = await call('debug_read_export', {**export_arguments, 'offset': '0', 'length': 64 * 48 * 4})
                            require(chunk['bytes']['encoding'] == 'base64' and not chunk['metadata_only'], chunk)
                            require(base64.b64decode(chunk['bytes']['data']) == b'\xff\x00\x00\xff' * (64 * 48),
                                    'Actual MCP explicit raw image pixels changed')
                            require((await call('debug_release_export', export_arguments))['released'], exported)
                    require((await call('debug_status', common))['pipeline']['permits'] == 0, 'MCP inspection admitted hidden work')
                    await call('debug_renew', {**common, 'lease': lease})
                    for capture in retained:
                        release, _ = await command('debug_release', capture=capture)
                        require((await operation(release['operation_id']))['progress'] == 'released', 'MCP Release failed')
                    await command('debug_resume')
                    output = (await asyncio.to_thread(probe.communicate, timeout=10))[0]
                    require(probe.returncode == 0, output)
                    await call('debug_disconnect', {'connection': common['connection']})
                    print(output)
                    print('PASS: actual MCP stdio, explicit command IDs, exact composed/raw/display capture, release/resume')
                    return 0
        finally:
            if probe.poll() is None:
                probe.kill()
                probe.wait()
            if probe.stdout:
                probe.stdout.close()


if __name__ == '__main__':
    raise SystemExit(asyncio.run(run()))
