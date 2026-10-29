#!/usr/bin/env python3
"""Exercise an existing desktop host and its native connector from a relocated package."""
import argparse
import base64
import ctypes
from io import BytesIO
import json
import os
import re
import select
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
from mcp_support import Client, eventually

from packaging_support import reap_children


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    parser.add_argument('--video-driver', choices=('wayland', 'x11'), required=True)
    parser.add_argument('--normal-close', action='store_true', help='Close the X11 window and verify normal teardown')
    args = parser.parse_args()
    if args.normal_close and args.video_driver != 'x11':
        parser.error('--normal-close requires X11')
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), 'Could not establish process cleanup')
    source = args.stage.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='simxr-desktop-package-') as temporary:
        root = Path(temporary)
        package = root / "moved Ω '$" / 'OpenXR Simulator'
        package.parent.mkdir()
        shutil.copytree(source, package)
        binary = package / 'bin'
        app = binary / 'simxr_companion'
        connector = binary / 'simxr_mcp_connector'
        env = dict(os.environ, PATH='', SDL_VIDEODRIVER=args.video_driver,
                   SIMXR_DATA_DIR=str(root / 'data'), SIMXR_COMPANION_SETTINGS=str(root / 'settings'),
                   SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   XDG_CONFIG_HOME=str(root / 'xdg-config'), XDG_DATA_HOME=str(root / 'xdg-data'),
                   SIMXR_DETERMINISTIC='0')
        for key in ('XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET', 'SIMXR_MCP_TOKEN'):
            env.pop(key, None)
        output = root / 'host.log'
        bridge_log = root / 'bridge.log'
        handles = []
        bridge = None
        host = None
        with output.open('wb') as app_log, bridge_log.open('wb') as log:
            try:
                host = subprocess.Popen([str(app), '--hidden', '--mcp', '--validation'], env=env, cwd=root,
                                        stdout=app_log, stderr=subprocess.STDOUT, start_new_session=True)
                def ready():
                    if host.poll() is not None:
                        raise AssertionError('Desktop host exited: ' + output.read_text(errors='replace'))
                    record = next((root / 'discovery').glob('*.json'), None)
                    return json.loads(record.read_text()) if record else None
                record = eventually(ready, timeout=30)
                assert int(record['process_id']) == host.pid, record
                printed = subprocess.run([str(connector), '--print-config'], env=env, cwd=root,
                                         capture_output=True, check=True, timeout=15)
                configured = json.loads(printed.stdout)['mcpServers']['openxr-simulator']
                assert Path(configured['command']) == connector and configured['args'] == [], configured

                def connect():
                    process = subprocess.Popen([configured['command'], *configured['args']], env=env, cwd=root,
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1,
                        start_new_session=True)
                    client = Client(process)
                    reply = client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                        'clientInfo': {'name': 'desktop-package-test', 'version': '1'}})
                    assert 'result' in reply, reply
                    client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                    return process, client

                bridge, client = connect()
                names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
                assert {'simulator_launch_application', 'simulator_luau_run', 'capture_screenshot'} <= names
                assert 'simulator_shutdown' not in names
                ids, pids = [], []
                for name in ('First', 'Second'):
                    data = root / ('runtime-' + name.lower())
                    data.mkdir()
                    launched = client.tool('simulator_launch_application', {'name': name,
                        'executable': str(binary / 'xr_probe_linux'), 'environment': {
                            'SIMXR_DATA_DIR': str(data), 'SIMXR_PROBE_FRAMES': '120000',
                            'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
                    assert not launched['error'], launched
                    identity = launched['data']['application_id']
                    ids.append(identity)
                    def connected():
                        status = client.tool('simulator_application_status', {'application_id': identity})
                        assert not status['error'] and status['data']['launch_state'] != 'failed', status
                        return status['data'] if status['data']['session_running'] else None
                    state = eventually(connected, timeout=40)
                    pid = int(state['process_id'])
                    assert str(binary / 'libopenxr_simulator.so') in Path(f'/proc/{pid}/maps').read_text()
                    handles.append(os.pidfd_open(pid))
                    pids.append(pid)
                assert ids[0] != ids[1] and pids[0] != pids[1]
                def preview():
                    if host.poll() is not None:
                        raise AssertionError('Desktop host exited before preview: ' + output.read_text(errors='replace'))
                    text = output.read_text(errors='replace')
                    return 'Companion retained GPU image' in text and 'normalReadback=0' in text
                eventually(preview, timeout=30)
                assert f'SDL driver: {args.video_driver}' in output.read_text(errors='replace')
                run = client.tool('simulator_luau_run', {'application_id': ids[0],
                    'source': "simxr.checkpoint('desktop package', true)"})
                assert not run['error'], run
                def finished():
                    status = client.tool('simulator_luau_status', {'application_id': ids[0],
                        'run_id': run['data']['run_id']})
                    assert not status['error'], status
                    return status['data'] if status['data']['phase'] not in ('running', 'paused') else None
                result = eventually(finished, timeout=30)
                assert result['phase'] == 'complete' and result['checkpoints'][0]['passed'], result
                image = client.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': {
                    'application_id': ids[1], 'eye': 'left', 'format': 'png', 'max_width': 64, 'timeout': 20}})['result']
                assert not image['isError'], image
                from PIL import Image
                with Image.open(BytesIO(base64.b64decode(image['content'][1]['data'], validate=True))) as pixels:
                    pixels.load()
                    assert pixels.format == 'PNG' and pixels.size == (64, 48)
                    assert max(value[1] for value in pixels.convert('RGB').getextrema()) > 0
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                bridge = None
                assert host.poll() is None
                for handle in handles:
                    assert not select.select([handle], [], [], 0)[0], 'A launched XR process exited after connector shutdown'
                bridge, client = connect()
                listing = client.tool('simulator_list_applications', {})
                assert not listing['error'], listing
                found = {item['application_id'] for item in listing['data']['applications']}
                assert set(ids) <= found, listing
                if args.normal_close:
                    from x11_close import close_process_window
                    close_process_window(host.pid)
                    assert host.wait(timeout=30) == 0, 'Desktop host failed during normal window close'
                    assert not select.select(handles, [], [], 2)[0], 'An XR process did not survive desktop close'
                    def unavailable():
                        reply = client.rpc('tools/list')
                        return reply if 'error' in reply else None
                    eventually(unavailable, timeout=15)
                    assert bridge.poll() is None, 'Connector exited with the desktop host'
                else:
                    for identity in ids:
                        stopped = client.tool('simulator_stop_application', {'application_id': identity})
                        assert not stopped['error'], stopped
                    for handle in handles:
                        assert select.select([handle], [], [], 10)[0], 'A stopped XR process did not exit'
                    assert host.poll() is None, 'Desktop host exited while stopping XR processes'
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                bridge = None
                app_log.flush()
                text = output.read_text(errors='replace')
                assert 'VUID-' not in text and 'Validation Error' not in text and 'preserving' not in text and 'FAIL line' not in text, text
                if args.normal_close:
                    adapter = next((line for line in text.splitlines() if line.startswith('Companion adapter: ')), None)
                    summary = next((line for line in text.splitlines() if re.fullmatch(
                        r'Companion: [0-9]+ UI submissions, [0-9]+ swapchain creations, 0 validation errors; runtime connection mode=interactive', line)), None)
                    assert adapter and summary, text
            except Exception:
                app_log.flush()
                log.flush()
                print(output.read_text(errors='replace')[-6000:], file=sys.stderr)
                print(bridge_log.read_text(errors='replace')[-2000:], file=sys.stderr)
                raise
            finally:
                try:
                    if bridge and bridge.poll() is None:
                        bridge.kill()
                        bridge.wait(timeout=10)
                    if host and host.poll() is None:
                        os.killpg(host.pid, signal.SIGTERM)
                        try:
                            host.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(host.pid, signal.SIGKILL)
                            host.wait(timeout=10)
                    for handle in handles:
                        try:
                            signal.pidfd_send_signal(handle, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                        finally:
                            os.close(handle)
                finally:
                    reap_children()
        if args.normal_close:
            print(adapter)
            print(summary)
        outcome = ('normal host teardown and independent game survival' if args.normal_close else
                   'desktop host stopped by signal, normal teardown not checked')
        print(f'PASS relocated {args.video_driver} live desktop host, MCP reconnect, two XR apps, GPU preview, Luau and PNG; {outcome}')


if __name__ == '__main__':
    main()
