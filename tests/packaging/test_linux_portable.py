#!/usr/bin/env python3
"""Exercise a relocated Linux payload through its adjacent native connector."""

import argparse
import base64
import ctypes
from io import BytesIO
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
from mcp_support import Client, eventually


from packaging_support import reap_children

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', type=Path)
    parser.add_argument('--skip-image', action='store_true', help='Do not require GPU image export')
    args = parser.parse_args()
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'Could not establish process cleanup')
    source = args.stage.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='simxr-portable-') as temporary:
        root = Path(temporary)
        initial = root / 'original'
        moved = root / 'moved Ω $' / 'OpenXR Simulator'
        moved.parent.mkdir()
        shutil.copytree(source, initial)
        initial.rename(moved)
        env = dict(os.environ, HOME=str(root), PATH='', SIMXR_DATA_DIR=str(root / 'data'),
                   SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_DETERMINISTIC='0',
                   SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_MCP_TOKEN'):
            env.pop(key, None)
        connector = moved / 'bin/simxr_mcp_connector'
        printed = subprocess.run([str(connector), '--print-config', '--windowless'], env=env, cwd=root,
                                 capture_output=True, check=True, timeout=15)
        configured = json.loads(printed.stdout)['mcpServers']['openxr-simulator']
        assert Path(configured['command']) == connector and configured['args'] == ['--windowless'], configured
        assert not (root / 'discovery').exists(), 'Configuration generation must not start the host'
        with (root / 'host.log').open('wb') as log:
            bridge = subprocess.Popen([configured['command'], *configured['args']], env=env, cwd=root,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                      text=True, bufsize=1, start_new_session=True)
            try:
                client = Client(bridge)
                init = client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                                'clientInfo': {'name': 'portable-test', 'version': '1'}})
                assert 'result' in init, init
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
                assert {'simulator_launch_application', 'capture_screenshot', 'simulator_luau_run'} <= names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                assert int(json.loads(discovery.read_text())['process_id']) > 0
                launched = client.tool('simulator_launch_application', {
                    'executable': str(moved / 'bin/xr_probe_linux'),
                    'environment': {'SIMXR_PROBE_FRAMES': '120000', 'SIMXR_PROBE_INTERACTIVE': '1',
                                    'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']

                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'], status
                    assert status['data']['launch_state'] != 'failed', status
                    return status['data'] if status['data']['session_running'] else None

                state = eventually(connected, timeout=30)
                maps = Path(f"/proc/{state['process_id']}/maps").read_text()
                assert str(moved / 'bin/libopenxr_simulator.so') in maps, maps
                script = client.tool('simulator_luau_run', {'application_id': application,
                                     'source': "simxr.checkpoint('portable run', true)"})
                assert not script['error'], script

                def finished():
                    result = client.tool('simulator_luau_status', {'application_id': application,
                                         'run_id': script['data']['run_id']})
                    assert not result['error'], result
                    return result['data'] if result['data']['phase'] not in ('running', 'paused') else None

                result = eventually(finished)
                assert result['phase'] == 'complete' and result['checkpoints'][0]['passed'], result

                if not args.skip_image:
                    last_screenshot = None

                    def screenshot():
                        nonlocal last_screenshot
                        last_screenshot = client.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': {
                            'application_id': application, 'eye': 'left', 'format': 'png', 'max_width': 64}})
                        assert 'result' in last_screenshot, last_screenshot
                        result = last_screenshot['result']
                        return result if not result.get('isError') else None

                    try:
                        content = eventually(screenshot, timeout=30)['content']
                    except AssertionError as error:
                        raise AssertionError(f'PNG capture failed: {last_screenshot}') from error
                    assert content[1]['type'] == 'image' and content[1]['mimeType'] == 'image/png', content
                    from PIL import Image
                    with Image.open(BytesIO(base64.b64decode(content[1]['data'], validate=True))) as image:
                        image.load()
                        assert image.format == 'PNG' and image.size == (64, 48), (image.format, image.size)
                        assert max(channel[1] for channel in image.convert('RGB').getextrema()) > 0
                assert not client.tool('simulator_stop_application', {'application_id': application})['error']
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
            finally:
                if bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                reap_children()
        print('PASS relocated Linux package, automatic MCP startup, XR launch and Luau' +
              ('; PNG capture skipped' if args.skip_image else ' and PNG capture'))


if __name__ == '__main__':
    main()
