#!/usr/bin/env python3
"""Exercise native MCP one-call screenshots against Windows Vulkan XR."""

import base64
from io import BytesIO
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image
from test_mcp_connector import Client, eventually
from test_mcp_discovery_windows import kernel, process_handle


def main():
    app, connector, probe, manifest = (str(Path(value).resolve()) for value in sys.argv[1:5])
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-screenshot-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'), SIMXR_DATA_DIR=temporary,
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        env.pop('SIMXR_MCP_TOKEN', None)
        env.pop('XR_RUNTIME_JSON', None)
        handles = []
        with (root / 'bridge.log').open('wb') as log:
            bridge = subprocess.Popen([connector, '--app', app, '--windowless'], env=env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8',
                bufsize=1, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
            try:
                client = Client(bridge)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'windows-screenshot-test', 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                def discovery():
                    files = list((root / 'discovery').glob('*.json'))
                    return json.loads(files[0].read_text(encoding='utf-8')) if len(files) == 1 else None
                handles.append(process_handle(int(eventually(discovery)['process_id'])))
                endpoint = rf'\\.\pipe\SimXR.Control.mcp.screenshot.{os.getpid()}'
                launched = client.tool('simulator_launch_application', {'executable': probe, 'runtime_manifest': manifest,
                    'environment': {'SIMXR_CONTROL_SOCKET': endpoint, 'XR_PROBE_FRAMES': '120000',
                    'XR_PROBE_SLEEP_MS': '1', 'XR_PROBE_PROJECTION_ONLY': '1',
                    'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror': '1',
                    'DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY': '1'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']
                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'] and status['data']['launch_state'] not in ('failed', 'exited'), status
                    return status['data'] if status['data']['session_running'] else None
                state = eventually(connected, timeout=45)
                handles.append(process_handle(int(state['process_id'])))
                opened = client.tool('debug_connect', {'application_id': application})
                assert not opened['error'], opened
                connection = opened['data']['connection']
                def displayed():
                    preview = client.tool('debug_retain_preview', {'connection': connection})
                    if preview['error'] or not preview['data'].get('image_available'):
                        return False
                    assert not client.tool('debug_release_image', {'connection': connection,
                                           'image': preview['data']['image']})['error']
                    return True
                eventually(displayed, timeout=30)
                for layer in ('all', 'projection', 'quad'):
                    response = client.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': {
                        'application_id': application, 'layer': layer, 'eye': 'both', 'format': 'png',
                        'max_width': 64, 'timeout': 15}})
                    result = response['result']
                    assert not result['isError'], (layer, result)
                    content = result['content']
                    metadata = json.loads(content[0]['text'])
                    assert metadata['frame']['generation'] == state['generation'] and metadata['layer'] == layer, metadata
                    if layer != 'all':
                        assert metadata['composition']['frame'] == metadata['frame'], metadata
                    assert [eye['eye'] for eye in metadata['eyes']] == ['left', 'right'] and len(content) == 3
                    for item, eye in zip(content[1:], metadata['eyes']):
                        assert item['type'] == 'image' and item['mimeType'] == 'image/png', item
                        with Image.open(BytesIO(base64.b64decode(item['data'], validate=True))) as decoded:
                            assert decoded.size == (eye['width'], eye['height']) and decoded.width <= 64
                            decoded.load()
                stereo = client.tool('validate_stereo', {'application_id': application, 'timeout': 15})
                assert not stereo['error'], stereo
                assert stereo['data']['frame']['generation'] == state['generation'], stereo
                assert stereo['data']['composition']['frame'] == stereo['data']['frame'], stereo
                assert stereo['data']['verdict'] in ('PASS', 'FAIL_NO_PARALLAX', 'FAIL_EXCESSIVE_PARALLAX',
                    'INCONCLUSIVE_NO_FEATURES', 'INCONCLUSIVE_NO_MATCH', 'INCONCLUSIVE_AMBIGUOUS'), stereo
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                print('PASS Windows Vulkan XR screenshots and same-frame stereo validation')
            finally:
                if bridge.poll() is None:
                    bridge.kill()
                    bridge.wait()
                for handle in handles:
                    if kernel.WaitForSingleObject(handle, 0) == 0x102:
                        kernel.TerminateProcess(handle, 1)
                    kernel.CloseHandle(handle)


if __name__ == '__main__':
    main()
