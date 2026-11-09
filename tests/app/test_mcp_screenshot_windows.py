#!/usr/bin/env python3
"""Exercise native MCP one-call screenshots against Windows Vulkan XR."""

import base64
from io import BytesIO
import json
import os
from pathlib import Path
import sys
from PIL import Image

from mcp_support import Client, McpTest, eventually


def main():
    app, connector, probe, manifest = (str(Path(value).resolve()) for value in sys.argv[1:5])
    with McpTest('simxr-mcp-screenshot-', windowless=True) as test:
        root = test.root

        log = test.log('bridge.log')
        bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        client = Client(bridge)
        client.initialize('windows-screenshot-test')

        test.discover(client)
        launched = client.tool('simulator_launch_application', {'executable': probe, 'runtime_manifest': manifest,
            'environment': {'SIMXR_PROBE_FRAMES': '120000',
            'SIMXR_PROBE_SLEEP_MS': '1', 'SIMXR_PROBE_PROJECTION_ONLY': '1',
            'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror': '1',
            'DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY': '1'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        state = test.connected(client, application, timeout=45)

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
        for layer in ('current_display', 'projection_layers', 'quad_layers'):
            response = client.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': {
                'application_id': application, 'target': layer, 'eye': 'both', 'format': 'png',
                'max_width': 64, 'timeout': 15}})
            result = response['result']
            assert not result['isError'], (layer, result)
            content = result['content']
            metadata = json.loads(content[0]['text'])
            assert metadata['frame']['generation'] == state['generation'] and metadata['target'] == layer, metadata
            if layer != 'current_display':
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


if __name__ == '__main__':
    main()
