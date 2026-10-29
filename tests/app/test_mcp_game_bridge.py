#!/usr/bin/env python3
"""Exercise native MCP against the real C game bridge without an XR session."""

import json
import http.client
import os
from pathlib import Path
import signal
import secrets
import subprocess
import sys
import tempfile
import time

from mcp_support import Client, eventually

if os.name == 'nt':
    from ctypes import wintypes
    from mcp_support import kernel, process_handle

    kernel.WaitNamedPipeW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD)
    kernel.WaitNamedPipeW.restype = wintypes.BOOL


def cancel_unadmitted(discovery, application, game):
    record = json.loads(next(discovery.glob('*.json')).read_text(encoding='utf-8'))
    headers = {'Authorization': 'Bearer ' + record['token'], 'Accept': 'application/json, text/event-stream',
               'Content-Type': 'application/json'}

    def request(method, message=None, extra=None):
        connection = http.client.HTTPConnection('127.0.0.1', record['port'], timeout=8)
        try:
            connection.request(method, '/mcp', None if message is None else json.dumps(message), dict(headers, **(extra or {})))
            response = connection.getresponse()
            return response.status, response.read(), response.getheader('MCP-Session-Id')
        finally:
            connection.close()

    status, body, session = request('POST', {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
        'params': {'protocolVersion': '2025-11-25', 'capabilities': {}, 'clientInfo': {'name': 'cancel-test', 'version': '1'}}})
    assert status == 200 and session and 'result' in json.loads(body), (status, body)
    headers['MCP-Session-Id'] = session
    status, body, _ = request('POST', {'jsonrpc': '2.0', 'method': 'notifications/initialized'})
    assert status == 202 and not body
    status, body, _ = request('POST', {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/call',
        'params': {'name': 'game_bridge_connect', 'arguments': {'application_id': application}}})
    result = json.loads(body)['result']
    assert status == 200 and not result.get('isError'), result
    connection = json.loads(result['content'][0]['text'])['connection']
    key = secrets.token_hex(32)
    status, body, _ = request('POST', {'jsonrpc': '2.0', 'method': 'notifications/cancelled',
        'params': {'requestId': 17}}, {'X-Simxr-Cancellation-Key': key})
    assert status == 202 and not body
    status, body, _ = request('POST', {'jsonrpc': '2.0', 'id': 17, 'method': 'tools/call',
        'params': {'name': 'game_bridge_command', 'arguments': {'connection': connection, 'game': game,
        'command_id': '1', 'kind': 1, 'payload': 'checkpoint:start'}}}, {'X-Simxr-Cancellation-Key': key})
    assert status == 202 and not body, (status, body)
    return lambda: request('DELETE')


def main():
    app, connector, probe, runtime = (str(Path(value).resolve()) for value in sys.argv[1:5])
    visible = '--visible' in sys.argv[5:]
    long_wait = '--long-wait' in sys.argv[5:]
    assert set(sys.argv[5:]) <= {'--visible', '--long-wait'}, sys.argv[5:]
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-game-Ü-') as temporary:
        root = Path(temporary)
        endpoint = (rf'\\.\pipe\SimXR.Control.mcp.game.{os.getpid()}.{secrets.token_hex(8)}'
                    if os.name == 'nt' else str(root / 'game.sock'))
        stop = root / 'stop'
        env = dict(os.environ, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'), SIMXR_DATA_DIR=temporary,
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'))
        if not visible:
            env['VK_DRIVER_FILES'] = '/nonexistent/no-vulkan.json'
        env.pop('SIMXR_CONTROL_SOCKET', None)
        env.pop('XR_RUNTIME_JSON', None)
        app_pid = game_pid = None
        handles = []
        spawn_options = ({'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt'
                         else {'start_new_session': True})
        with (root / 'connector.log').open('wb') as log:
            application_process = subprocess.Popen([app, '--mcp', '--hidden'], env=env, stdin=subprocess.DEVNULL,
                                                   stdout=log, stderr=log) if visible else None
            if application_process:
                def visible_ready():
                    assert application_process.poll() is None, 'Visible host exited before publishing MCP discovery'
                    return next((path for path in (root / 'discovery').glob('*.json')), None)
                try:
                    eventually(visible_ready, timeout=20)
                except BaseException:
                    if application_process.poll() is None:
                        application_process.terminate()
                        application_process.wait(timeout=10)
                    raise
            connector_command = [connector, '--app', app] + ([] if visible else ['--windowless'])
            bridge = subprocess.Popen(connector_command, env=env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8', bufsize=1, **spawn_options)
            second = None
            close_cancel_session = None
            try:
                client = Client(bridge)
                initialized = client.initialize('game-test')
                assert 'result' in initialized, initialized

                tools = {tool['name'] for tool in client.rpc('tools/list')['result']['tools']}
                assert {'game_bridge_connect', 'game_bridge_status', 'game_bridge_observations',
                        'game_bridge_command', 'game_bridge_operation'} <= tools
                app_pid = int(eventually(lambda: next((json.loads(p.read_text(encoding='utf-8'))['process_id']
                    for p in (root / 'discovery').glob('*.json')), None)))
                if os.name == 'nt':
                    handles.append(process_handle(app_pid))
                launched = client.tool('simulator_launch_application', {'executable': probe,
                    'arguments': [runtime, endpoint, str(stop)], 'environment': {'SIMXR_CONTROL_SOCKET': endpoint,
                    'VK_DRIVER_FILES': '/nonexistent/no-vulkan.json',
                    'SIMXR_GAME_PROBE_SECONDS': '60' if long_wait else '10'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']

                def ready():
                    state = client.tool('simulator_application_status', {'application_id': application})
                    assert not state['error'], state
                    present = bool(kernel.WaitNamedPipeW(endpoint, 0)) if os.name == 'nt' else Path(endpoint).exists()
                    return state['data'] if state['data']['launch_state'] == 'running' and present else None

                state = eventually(ready, timeout=8)
                game_pid = int(state['process_id'])
                if os.name == 'nt':
                    handles.append(process_handle(game_pid))
                assert not state['session_running'] and state['launch_endpoint'] == endpoint, state
                if long_wait:
                    time.sleep(32)
                opened = client.tool('game_bridge_connect', {'application_id': application})
                assert not opened['error'] and opened['data']['process_id'] == str(game_pid), opened
                connection = opened['data']['connection']
                assert client.tool('debug_status', {'connection': connection})['error']
                def registered(connection_id):
                    status = client.tool('game_bridge_status', {'connection': connection_id})
                    if status['error'] or status['data']['document']['capabilities'] != 1:
                        return None
                    return status['data']

                game = eventually(lambda: registered(connection), timeout=5)['game_id']
                close_cancel_session = cancel_unadmitted(root / 'discovery', application, game)
                command = {'connection': connection, 'game': game, 'command_id': '9007199254740993',
                           'kind': 1, 'payload': 'checkpoint:start'}
                queued = client.tool('game_bridge_command', command)
                assert not queued['error'], queued
                duplicate = client.tool('game_bridge_command', command)
                assert not duplicate['error'] and duplicate['data']['operation_id'] == queued['data']['operation_id'], duplicate
                changed = client.tool('game_bridge_command', dict(command, payload='different'))
                assert changed['error'], changed
                assert client.tool('game_bridge_observations', {'connection': connection, 'count': 17})['error']
                assert client.tool('game_bridge_command', dict(command, command_id='2', payload='x' * 4097))['error']

                second = subprocess.Popen(connector_command, env=env, stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8', bufsize=1, **spawn_options)
                other = Client(second)
                other.initialize('other-game-test')

                assert other.tool('game_bridge_status', {'connection': connection})['error']
                other_open = other.tool('game_bridge_connect', {'application_id': application})
                assert not other_open['error'], other_open
                other_connection = other_open['data']['connection']
                operation = {'connection': connection, 'game': game, 'operation': queued['data']['operation_id']}
                assert other.tool('game_bridge_operation', dict(operation, connection=other_connection))['error']

                def complete():
                    result = client.tool('game_bridge_operation', operation)
                    assert not result['error'], result
                    return result['data'] if result['data']['phase'] == 'complete' else None

                result = eventually(complete, timeout=5)
                assert result['document']['result'] == 'reset applied', result
                assert close_cancel_session()[0] == 204
                close_cancel_session = None
                observations = client.tool('game_bridge_observations', {'connection': connection, 'offset': '0', 'count': 16})
                assert not observations['error'], observations
                events = observations['data']['document']['observations']
                assert [event['kind'] for event in events] == ['registered', 'telemetry'], events
                assert json.loads(events[1]['payload'])['reset_count'] == 1, events
                assert events[1]['game_id'] == game and events[1]['observed_time']['nanoseconds'] > 0, events
                assert not client.tool('debug_disconnect', {'connection': connection})['error']
                assert not other.tool('game_bridge_status', {'connection': other_connection})['error']
                if not visible:
                    restarted = client.tool('simulator_restart_application', {'application_id': application,
                                            'process_id': str(game_pid)})
                    assert not restarted['error'] and restarted['data']['accepted'], restarted
                    previous_pid = game_pid

                    def replacement():
                        result = client.tool('simulator_application_status', {'application_id': application})
                        assert not result['error'], result
                        data = result['data']
                        return data if data['launch_state'] == 'running' and int(data['process_id']) != previous_pid else None

                    game_pid = int(eventually(replacement, timeout=8)['process_id'])
                    if os.name == 'nt':
                        handles.append(process_handle(game_pid))
                    assert other.tool('game_bridge_status', {'connection': other_connection})['error']

                    def connect_replacement():
                        result = client.tool('game_bridge_connect', {'application_id': application})
                        return result['data'] if not result['error'] else None

                    new_connection = eventually(connect_replacement, timeout=8)['connection']
                    new_game = eventually(lambda: registered(new_connection), timeout=5)['game_id']
                    new_command = client.tool('game_bridge_command', {'connection': new_connection, 'game': new_game,
                        'command_id': '1', 'kind': 1, 'payload': 'checkpoint:start'})
                    assert not new_command['error'], new_command

                    def new_complete():
                        result = client.tool('game_bridge_operation', {'connection': new_connection, 'game': new_game,
                                             'operation': new_command['data']['operation_id']})
                        assert not result['error'], result
                        return result['data'] if result['data']['phase'] == 'complete' else None

                    assert eventually(new_complete, timeout=5)['document']['result'] == 'reset applied'
                stop.touch()

                def exited():
                    result = client.tool('simulator_application_status', {'application_id': application})
                    return result['data'] if not result['error'] and result['data']['launch_state'] == 'exited' else None

                assert eventually(exited, timeout=5)['exit_code'] == 0
                game_pid = None
                if visible:
                    assert other.tool('game_bridge_status', {'connection': other_connection})['error']
                else:
                    assert client.tool('game_bridge_status', {'connection': new_connection})['error']
                if not visible:
                    assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=10) == 0
                print('PASS native MCP C game bridge, process-only startup, ownership, retry identity and acknowledgement')
            finally:
                if close_cancel_session:
                    try:
                        close_cancel_session()
                    except OSError:
                        pass
                if second and second.poll() is None:
                    second.kill()
                    second.wait()
                if bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                if application_process and application_process.poll() is None:
                    application_process.terminate()
                    application_process.wait(timeout=10)
                if os.name == 'nt':
                    for handle in handles:
                        if kernel.WaitForSingleObject(handle, 0) == 0x102:
                            kernel.TerminateProcess(handle, 1)
                        kernel.CloseHandle(handle)
                else:
                    for pid in (app_pid, game_pid):
                        if pid:
                            try:
                                os.kill(pid, signal.SIGTERM)
                            except ProcessLookupError:
                                pass


if __name__ == '__main__':
    main()
