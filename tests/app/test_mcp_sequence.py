#!/usr/bin/env python3
"""Native MCP sequence execution, exact captures and owner cleanup on real XR frames."""
import base64
from io import BytesIO
import json
from pathlib import Path
import sys
from PIL import Image

from mcp_support import McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-mcp-sequence-', windowless=True) as test:
        root = test.root

        log = test.log('host.log')

        client = test.connect(connector, '--app', app, '--windowless', log=log, name='sequence-owner')
        names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
        assert {'upload_pose_sequence', 'run_pose_sequence', 'get_pose_sequence',
                'stop_pose_sequence', 'release_pose_sequence_capture'} <= names
        test.discover(client)

        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_RECORDING': '1', 'SIMXR_PROBE_GPU_INSPECTION': '1',
            'SIMXR_PROBE_DYNAMIC_COLOR': '1', 'SIMXR_PROBE_CANONICAL_SEQUENCE': '1', 'SIMXR_PROBE_FRAMES': '10',
            'SIMXR_WIDTH': '64', 'SIMXR_HEIGHT': '48'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        test.connected(client, application, timeout=30)

        eventually(lambda: (root / 'control.ready').exists())
        opened = client.tool('debug_connect', {'application_id': application})
        assert not opened['error'], opened
        connection = opened['data']['connection']
        other = test.connect(connector, '--app', app, '--windowless', log=log, name='sequence-observer')
        other_open = other.tool('debug_connect', {'application_id': application})
        assert not other_open['error'], other_open
        other_connection = other_open['data']['connection']
        steps = [{'frame': frame, 'head': {'x': .3 + .1 * frame}, 'capture': frame < 2}
                 for frame in range(4)]
        steps[0]['head'].update(y=1.8, z=-.4, yaw=.2, pitch=-.1, roll=.15)
        # Exercise inline upload across its 256 KiB chunk boundary.
        program = '\n'.join(json.dumps(step) for step in steps) + '\n' * (256 * 1024)
        request = {'connection': connection, 'program_id': '9007199254740993', 'program': program}
        started = client.tool('run_pose_sequence', request)
        assert not started['error'], started
        sequence = started['data']['sequence_id']
        repeated = client.tool('run_pose_sequence', request)
        assert not repeated['error'] and repeated['data']['sequence_id'] == sequence, repeated
        assert client.tool('run_pose_sequence', dict(request, program=program + ' '))['error']
        assert other.tool('stop_pose_sequence', {'connection': other_connection, 'sequence_id': sequence})['error']
        (root / 'control.continue').touch()
        eventually(lambda: (root / 'recording.3.ready').exists())
        def page():
            value = client.tool('get_pose_sequence', {'connection': connection, 'sequence_id': sequence})
            assert not value['error'], value
            return value['data']['document']
        def receipt():
            found = [capture for capture in page()['captures'] if capture['frame'] == '3']
            return found[0] if found and found[0]['phase'] == 'ready' else None
        capture = eventually(receipt)
        assert capture['admission'] == 'accepted' and capture['capture_id'] != '0', capture
        assert other.tool('release_pose_sequence_capture', {'connection': other_connection,
                          'capture': capture['capture_id']})['error']
        retained = client.tool('debug_retain_image', {'connection': connection, 'capture': capture['capture_id']})
        assert not retained['error'], retained
        reader = retained['data']['image']
        released = client.tool('release_pose_sequence_capture', {'connection': connection, 'capture': capture['capture_id']})
        assert not released['error'] and released['data']['released'], released
        again = client.tool('release_pose_sequence_capture', {'connection': connection, 'capture': capture['capture_id']})
        assert not again['error'] and not again['data']['released'], again
        info = client.tool('debug_image_info', {'connection': connection, 'image': reader})
        assert not info['error'] and info['data']['frame']['frame_id'] == '3', info
        for eye, expected in ((0, (255, 0, 0)), (1, (0, 0, 255))):
            ticket = client.tool('debug_start_export', {'connection': connection, 'image': reader, 'layer': eye})
            assert not ticket['error'], ticket
            ticket_id = ticket['data']['export']
            def ready():
                result = client.tool('debug_export_info', {'connection': connection, 'export': ticket_id})
                assert not result['error'], result
                return result['data'] if result['data']['phase'] == 'complete' else None
            eventually(ready)
            path = root / ('左眼.png' if eye == 0 else '右眼.png')
            save_args = {'connection': connection, 'export': ticket_id, 'path': str(path)}
            assert client.tool('debug_save_image', dict(save_args, session='1',
                               generation='18446744073709551615'))['error']
            saved = client.tool('debug_save_image', save_args)
            assert not saved['error'] and saved['data']['frame']['frame_id'] == '3' and not saved['data']['lossy'], saved
            with Image.open(path) as decoded:
                assert decoded.convert('RGB').getpixel((4, 4)) == expected
            first = path.read_bytes()
            assert client.tool('debug_save_image', save_args)['error'] and path.read_bytes() == first
            assert client.tool('debug_save_image', dict(save_args, quality=90))['error']
            assert other.tool('debug_save_image', dict(save_args, connection=other_connection))['error']
            replaced = client.tool('debug_save_image', dict(save_args, overwrite=True))
            assert not replaced['error'] and path.read_bytes() == first, replaced
            if eye == 0:
                jpeg = root / '左眼.jpg'
                lossy = client.tool('debug_save_image', {'connection': connection, 'export': ticket_id,
                                    'path': str(jpeg), 'quality': 90})
                assert not lossy['error'] and lossy['data']['lossy'] and lossy['data']['media_type'] == 'image/jpeg', lossy
                with Image.open(jpeg) as decoded:
                    pixel = decoded.convert('RGB').getpixel((4, 4))
                    assert pixel[0] > 240 and pixel[1] < 15 and pixel[2] < 15, pixel
            assert not list(root.glob(path.name + '.*')), 'Image temporary file was not retired'
            image = client.rpc('tools/call', {'name': 'debug_render_export', 'arguments':
                               {'connection': connection, 'export': ticket_id, 'format': 'png'}})['result']
            assert not image['isError'], image
            with Image.open(BytesIO(base64.b64decode(image['content'][1]['data'], validate=True))) as decoded:
                assert decoded.convert('RGB').getpixel((4, 4)) == expected
            assert not client.tool('debug_release_export', {'connection': connection, 'export': ticket_id})['error']
            assert client.tool('debug_save_image', dict(save_args, overwrite=True))['error']
            assert path.read_bytes() == first
            assert not client.tool('debug_status', {'connection': connection})['error']
        assert not client.tool('debug_release_image', {'connection': connection, 'image': reader})['error']
        (root / 'recording.3.continue').touch()
        eventually(lambda: (root / 'recording.4.ready').exists())
        replacement = '{"frame":0,"head":{"x":0.6},"capture":true}\n'
        data = replacement.encode()
        midpoint = len(data) // 2
        for offset, chunk in ((0, data[:midpoint]), (midpoint, data[midpoint:])):
            args = {'connection': connection, 'program_id': '9007199254740994', 'offset': offset,
                    'total_bytes': len(data), 'data': chunk.decode()}
            result = client.tool('upload_pose_sequence', args)
            assert not result['error'], result
            duplicate = client.tool('upload_pose_sequence', args)
            assert not duplicate['error'], duplicate
        started2 = client.tool('run_pose_sequence', {'connection': connection,
                               'program_id': '9007199254740994', 'loop': True})
        assert not started2['error'], started2
        next_sequence = started2['data']['sequence_id']
        assert next_sequence != sequence
        assert client.tool('stop_pose_sequence', {'connection': connection, 'sequence_id': sequence})['error']
        stopped = client.tool('stop_pose_sequence', {'connection': connection, 'sequence_id': next_sequence})
        assert not stopped['error'] and not stopped['data']['document']['active'], stopped
        assert not client.tool('debug_disconnect', {'connection': connection})['error']
        assert not other.tool('debug_disconnect', {'connection': other_connection})['error']
        assert not client.tool('simulator_stop_application', {'application_id': application})['error']
        assert not client.tool('simulator_shutdown', {})['error']
        for process in (client.process, other.process):
            process.stdin.close()
            assert process.wait(timeout=15) == 0
        print('PASS native sequence upload, retries, playback, exact captures, replacement and ownership')


if __name__ == '__main__':
    main()
