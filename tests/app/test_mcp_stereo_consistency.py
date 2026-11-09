#!/usr/bin/env python3
"""Exercise native stereo analysis through a windowless host and real XR process."""


from pathlib import Path
import sys
import time

from mcp_support import Client, McpTest, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with McpTest('simxr-stereo-mcp-', windowless=True) as test:
        root = test.root

        log = test.log('bridge.log')
        bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        other_bridge = None
        client = Client(bridge)
        client.initialize('stereo-test')

        names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
        assert 'analyze_stereo_consistency' in names
        test.discover(client)

        launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
            'SIMXR_PROBE_INTERACTIVE': '1', 'SIMXR_PROBE_FRAMES': '120000',
            'SIMXR_WIDTH': '128', 'SIMXR_HEIGHT': '96'}})
        assert not launched['error'], launched
        application = launched['data']['application_id']

        state = test.connected(client, application, timeout=30)

        assert client.tool('analyze_stereo_consistency', {'application_id': application, 'threshold': 0})['error']
        assert client.tool('analyze_stereo_consistency', {'application_id': application, 'frames': 0})['error']
        result = client.tool('analyze_stereo_consistency', {'application_id': application,
            'frames': 2, 'interval': .05, 'timeout': 15, 'include_image': False})
        assert not result['error'], result
        data = result['data']
        assert not data['incomplete'] and len(data['samples']) == 2, data
        for sample in data['samples']:
            assert sample['ok'] and sample['frame']['generation'] == state['generation'], sample
            assert sample['analysis_width'] == 128 and sample['analysis_height'] == 96, sample
            assert sample['verdict'].startswith('INCONCLUSIVE_'), sample
            assert sample['frusta_source'] == 'captured_image_views' and len(sample['image_views']) == 2, sample
            assert sample['infinity_disparity_pct'] is not None or sample['verdict'] == 'INCONCLUSIVE_NO_FEATURES'
        other_bridge = test.bridge(connector, '--app', app, '--windowless', log=log)
        worker = Client(other_bridge)
        worker.initialize('stereo-cancel')

        worker.send({'jsonrpc': '2.0', 'id': 901, 'method': 'tools/call', 'params': {
            'name': 'analyze_stereo_consistency', 'arguments': {'application_id': application,
                'frames': 30, 'interval': 1, 'timeout': 5, 'include_image': False}}})
        time.sleep(.5)
        worker.send({'jsonrpc': '2.0', 'method': 'notifications/cancelled', 'params': {'requestId': 901}})

        def available():
            result = client.tool('analyze_stereo_consistency', {'application_id': application,
                'timeout': 10, 'include_image': False})
            if result['error'] and ('busy' in result['data']['error'] or 'lease' in result['data']['error']):
                return None
            assert not result['error'], result
            return result

        eventually(available, timeout=20)
        other_bridge.stdin.close()
        assert other_bridge.wait(timeout=15) == 0

        assert not client.tool('simulator_stop_application', {'application_id': application})['error']
        assert not client.tool('simulator_shutdown', {})['error']
        bridge.stdin.close()
        assert bridge.wait(timeout=15) == 0
        print('PASS native MCP stereo sampling, exact image views and explicit inconclusive result')


if __name__ == '__main__':
    main()
