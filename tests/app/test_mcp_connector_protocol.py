#!/usr/bin/env python3
"""Check the installed connector contract independently of simulator tool versions."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from test_mcp_connector import Client, eventually


def main():
    app, connector = (str(Path(value).resolve(strict=True)) for value in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='simxr-connector-protocol-') as temporary:
        root = Path(temporary)
        discovery = root / 'discovery'
        env = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_COMPANION_SETTINGS=str(root / 'settings'),
                   SIMXR_MCP_DISCOVERY_DIR=str(discovery))
        env.pop('SIMXR_MCP_TOKEN', None)
        version = subprocess.run([connector, '--version'], capture_output=True, check=True, env=env, timeout=10)
        assert b'OpenXR Simulator MCP connector ' in version.stdout
        assert b'(connector protocol 1)' in version.stdout
        assert not discovery.exists(), 'Version reporting must not start the host'
        with (root / 'host.log').open('wb') as log:
            host = subprocess.Popen([app, '--windowless'], env=env, stdout=log, stderr=log)
            try:
                record = eventually(lambda: next(discovery.glob('*.json'), None))
                original = record.read_bytes()
                initial = json.loads(original)
                assert initial['connector_protocol'] == 1 and initial['process_id'] == host.pid
                for protocol in (2, '1', None, 1):
                    data = {'process_id': host.pid} if protocol == 2 else dict(initial)
                    if protocol is None:
                        del data['connector_protocol']
                    else:
                        data['connector_protocol'] = protocol
                    # Preserve the existing private file permissions/DACL.
                    record.write_text(json.dumps(data), encoding='utf-8')
                    bridge = subprocess.Popen([connector, '--app', app], env=env, stdin=subprocess.PIPE,
                                              stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8')
                    try:
                        client = Client(bridge)
                        result = client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                            'clientInfo': {'name': 'connector-protocol-check', 'version': '1'}})
                        if protocol in (None, 1):
                            assert 'result' in result, result
                            client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                            assert 'result' in client.rpc('tools/list')
                        else:
                            assert 'unsupported MCP connector protocol' in result['error']['message'], result
                        assert host.poll() is None
                        assert json.loads(record.read_bytes())['process_id'] == host.pid
                        if protocol == 1:
                            record.write_text(json.dumps({'connector_protocol': 2, 'process_id': host.pid}),
                                              encoding='utf-8')
                            eventually(lambda: 'unsupported MCP connector protocol' in
                                       client.rpc('tools/list').get('error', {}).get('message', ''))
                            assert host.poll() is None and bridge.poll() is None
                            record.write_bytes(original)
                            eventually(lambda: 'result' in client.rpc('tools/list'))
                            assert json.loads(record.read_bytes())['process_id'] == host.pid
                            assert not client.tool('simulator_shutdown', {})['error']
                        bridge.stdin.close()
                        assert bridge.wait(timeout=10) == 0
                    finally:
                        if bridge.poll() is None:
                            bridge.kill()
                            bridge.wait()
                assert host.wait(timeout=10) == 0
                print('PASS connector version, legacy/v1 discovery, incompatible refusal and recovery without respawn')
            finally:
                if host.poll() is None:
                    host.terminate()
                    host.wait(timeout=10)


if __name__ == '__main__':
    main()
