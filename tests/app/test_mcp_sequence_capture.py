#!/usr/bin/env python3
"""Native MCP sequence execution, exact captures and owner cleanup on real XR frames."""
import base64
from io import BytesIO
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

from PIL import Image
from test_mcp_connector import Client, eventually


def main():
    app, connector, probe = (str(Path(value).resolve()) for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix='simxr-mcp-sequence-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_DETERMINISTIC='0',
                   SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET'):
            env.pop(key, None)
        handles = []
        workers = []
        with (root / 'host.log').open('wb') as log:
            def connect(name):
                process = subprocess.Popen([connector, '--app', app, '--windowless'], env=env, stdin=subprocess.PIPE,
                                           stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1, start_new_session=True)
                workers.append(process)
                client = Client(process)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': name, 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                return client
            try:
                client = connect('sequence-owner')
                names = {item['name'] for item in client.rpc('tools/list')['result']['tools']}
                assert {'capture_pose_sequence', 'start_pose_sequence_capture', 'get_pose_sequence_capture',
                        'cancel_pose_sequence_capture'} <= names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                handles.append(os.pidfd_open(int(json.loads(discovery.read_text())['process_id'])))
                launched = client.tool('simulator_launch_application', {'executable': probe, 'environment': {
                    'SIMXR_PROBE_CONTROL': '1', 'SIMXR_PROBE_RECORDING': '1', 'SIMXR_PROBE_GPU_INSPECTION': '1',
                    'SIMXR_PROBE_DYNAMIC_COLOR': '1', 'SIMXR_PROBE_CANONICAL_SEQUENCE': '1', 'SIMXR_PROBE_FRAMES': '10',
                    'SIMXR_WIDTH': '64', 'SIMXR_HEIGHT': '48'}})
                assert not launched['error'], launched
                application = launched['data']['application_id']
                def connected():
                    status = client.tool('simulator_application_status', {'application_id': application})
                    assert not status['error'], status
                    return status['data'] if status['data']['session_running'] else None
                state = eventually(connected, timeout=30)
                handles.append(os.pidfd_open(int(state['process_id'])))
                eventually(lambda: (root / 'control.ready').exists())
                opened = client.tool('debug_connect', {'application_id': application})
                assert not opened['error'], opened
                connection = opened['data']['connection']
                from concurrent.futures import ThreadPoolExecutor
                import time
                steps = [{'frame': frame, 'head': {'x': .3 + .1 * frame}, 'capture': frame < 2}
                         for frame in range(4)]
                steps[0]['head'].update(y=1.8, z=-.4, yaw=.2, pitch=-.1, roll=.15)
                program = '\n'.join(json.dumps(step) for step in steps)
                output_root = root / '序列输出'
                output_root.mkdir()
                job = {'job_id': 'capture-1', 'connection': connection, 'program_id': '9007199254740993',
                       'program': program, 'directory': str(output_root), 'timeout': 15}
                for oversized in (4294967297, 18446744073709551615):
                    invalid = client.tool('start_pose_sequence_capture', dict(job, max_captures=oversized))
                    assert invalid['error'] and not list(output_root.iterdir()), invalid
                    synchronous = {key: value for key, value in job.items() if key != 'job_id'}
                    invalid = client.tool('capture_pose_sequence', dict(synchronous, max_captures=oversized))
                    assert invalid['error'] and not list(output_root.iterdir()), invalid
                started = client.tool('start_pose_sequence_capture', job)
                assert not started['error'] and started['data']['phase'] in ('starting', 'running'), started
                assert not client.tool('start_pose_sequence_capture', job)['error']
                assert client.tool('start_pose_sequence_capture', dict(job, timeout=14))['error']
                other = connect('sequence-observer')
                assert other.tool('get_pose_sequence_capture', {'job_id': 'capture-1'})['error']
                assert other.tool('cancel_pose_sequence_capture', {'job_id': 'capture-1'})['error']
                def running():
                    status = client.tool('get_pose_sequence_capture', {'job_id': 'capture-1'})
                    assert not status['error'] and status['data']['phase'] not in ('failed', 'cancelled'), status
                    return status['data']['capture'] if status['data']['phase'] == 'running' else None
                progress = eventually(running)
                manifest = Path(progress['directory']) / 'manifest.json'
                assert manifest.exists()
                assert client.tool('start_pose_sequence_capture', dict(job, job_id='different'))['error']
                (root / 'control.continue').touch()
                for frame in range(3, 7):
                    eventually(lambda frame=frame: (root / f'recording.{frame}.ready').exists())
                    if frame < 5:
                        def saved():
                            data = client.tool('get_pose_sequence_capture', {'job_id': 'capture-1'})
                            assert not data['error'] and data['data']['phase'] != 'failed', data
                            return data['data']['capture'] if len(data['data']['capture']['captures']) >= frame - 2 else None
                        eventually(saved)
                    (root / f'recording.{frame}.continue').touch()
                def completed():
                    status = client.tool('get_pose_sequence_capture', {'job_id': 'capture-1'})
                    assert not status['error'] and status['data']['phase'] != 'failed', status
                    return status['data'] if status['data']['phase'] == 'complete' else None
                result = eventually(completed)
                assert result['ok'], result
                report = result['capture']
                assert not client.tool('start_pose_sequence_capture', job)['error']
                assert json.loads(manifest.read_text()) == report
                assert [item['frame'] for item in report['captures']] == ['3', '4'], report
                for item in report['captures']:
                    with Image.open(item['path']) as decoded:
                        rgba = decoded.convert('RGB')
                        assert rgba.size == (128, 48)
                        expected = (255, 0, 0) if item['frame'] == '3' else (0, 255, 0)
                        assert rgba.getpixel((4, 4)) == expected
                        assert rgba.getpixel((68, 4)) == (0, 0, 255)
                assert not list(manifest.parent.glob('*.tmp'))
                if os.geteuid() != 0:
                    failing_program = '\n'.join(json.dumps({'frame': frame, 'head': {'x': .6}, 'capture': True})
                                                for frame in range(3))
                    with ThreadPoolExecutor(max_workers=1) as executor:
                        failing = executor.submit(client.tool, 'capture_pose_sequence', {'connection': connection,
                            'program_id': '9007199254740994', 'program': failing_program,
                            'directory': str(output_root), 'timeout': 15})
                        def failure_running():
                            for path in output_root.glob('sequence-*/manifest.json'):
                                data = json.loads(path.read_text())
                                if data['program_id'] == '9007199254740994' and data['phase'] == 'running':
                                    return path
                            return None
                        failure_manifest = eventually(failure_running)
                        failure_manifest.parent.chmod(0o500)
                        try:
                            for frame in range(7, 10):
                                (root / f'recording.{frame}.continue').touch()
                            failed = failing.result(timeout=15)
                        finally:
                            failure_manifest.parent.chmod(0o700)
                    assert failed['error'] and failed['data']['phase'] == 'failed', failed
                    assert len(failed['data']['unsaved_captures']) >= 2, failed
                    after_failure = client.tool('get_pose_sequence', {'connection': connection,
                        'sequence_id': failed['data']['sequence_id']})
                    assert not after_failure['error'] and not after_failure['data']['document']['held_captures'], after_failure
                else:
                    print('SKIP permission-based sequence write failure when running as root')
                loop_program = '{"frame":0,"head":{"x":0.6},"capture":true}\n'
                with ThreadPoolExecutor(max_workers=1) as executor:
                    loop = executor.submit(client.tool, 'capture_pose_sequence', {'connection': connection,
                        'program_id': '9007199254740995', 'program': loop_program, 'loop': True,
                        'directory': str(output_root), 'max_captures': 1, 'timeout': 15})
                    def loop_running():
                        for path in output_root.glob('sequence-*/manifest.json'):
                            data = json.loads(path.read_text())
                            if data['program_id'] == '9007199254740995' and data['phase'] == 'running':
                                return path
                        return None
                    loop_manifest = eventually(loop_running)
                    gate = 10 if os.geteuid() != 0 else 7
                    eventually(lambda: (root / f'recording.{gate}.ready').exists())
                    (root / f'recording.{gate}.continue').touch()
                    limited = loop.result(timeout=15)
                assert limited['error'] and limited['data']['phase'] == 'capture_limit', limited
                assert limited['data']['incomplete'] and len(limited['data']['captures']) == 1, limited
                assert not limited['data']['status']['active'], limited
                assert json.loads(loop_manifest.read_text()) == limited['data']
                held = client.tool('get_pose_sequence', {'connection': connection,
                                   'sequence_id': limited['data']['sequence_id']})
                assert not held['error'] and not held['data']['document']['held_captures'], held
                timed = client.tool('capture_pose_sequence', {'connection': connection,
                    'program_id': '9007199254740996', 'program': loop_program,
                    'directory': str(output_root), 'timeout': 0.25})
                assert timed['error'] and timed['data']['incomplete'] and timed['data']['phase'] == 'failed', timed
                assert 'timed out' in timed['data']['error'], timed
                assert json.loads((Path(timed['data']['directory']) / 'manifest.json').read_text()) == timed['data']
                assert client.tool('debug_status', {'connection': connection})['error']

                reopened = client.tool('debug_connect', {'application_id': application})
                assert not reopened['error'], reopened
                cancel_connection = reopened['data']['connection']
                cancel_job = {'job_id': 'cancel-job', 'connection': cancel_connection, 'program_id': '9007199254740997',
                              'program': loop_program, 'loop': True, 'directory': str(output_root), 'timeout': 15}
                assert not client.tool('start_pose_sequence_capture', cancel_job)['error']
                assert client.tool('start_pose_sequence_capture', job)['error']  # Earlier job ID is retired.
                def cancel_running():
                    status = client.tool('get_pose_sequence_capture', {'job_id': 'cancel-job'})
                    assert not status['error'] and status['data']['phase'] != 'failed', status
                    return status['data'] if status['data']['phase'] == 'running' else None
                cancel_status = eventually(cancel_running)
                cancel_manifest = Path(cancel_status['capture']['directory']) / 'manifest.json'
                cancelled = client.tool('cancel_pose_sequence_capture', {'job_id': 'cancel-job'})
                assert not cancelled['error'] and cancelled['data']['phase'] == 'cancelled', cancelled
                assert json.loads(cancel_manifest.read_text())['phase'] == 'cancelled'
                assert client.tool('debug_status', {'connection': cancel_connection})['error']
                assert client.tool('start_pose_sequence_capture', cancel_job)['data']['phase'] == 'cancelled'

                opened_other = other.tool('debug_connect', {'application_id': application})
                assert not opened_other['error'], opened_other
                owner_job = dict(cancel_job, job_id='disconnect-job', connection=opened_other['data']['connection'],
                                 program_id='9007199254740998')
                assert not other.tool('start_pose_sequence_capture', owner_job)['error']
                def owner_running():
                    status = other.tool('get_pose_sequence_capture', {'job_id': 'disconnect-job'})
                    assert not status['error'] and status['data']['phase'] != 'failed', status
                    return status['data'] if status['data']['phase'] == 'running' else None
                owner_status = eventually(owner_running)
                owner_manifest = Path(owner_status['capture']['directory']) / 'manifest.json'
                workers[1].stdin.close()
                assert workers[1].wait(timeout=15) == 0
                assert eventually(lambda: json.loads(owner_manifest.read_text())['phase'] == 'cancelled')
                assert not client.tool('simulator_stop_application', {'application_id': application})['error']
                assert not client.tool('simulator_shutdown', {})['error']
                for process in workers:
                    if not process.stdin.closed:
                        process.stdin.close()
                    assert process.wait(timeout=15) == 0
                print('PASS native sequence capture persistence, exact stereo PNGs and manifest')
            finally:
                if sys.exc_info()[0] is not None:
                    print((root / 'host.log').read_text(errors='replace'), file=sys.stderr)
                for process in workers:
                    if process.poll() is None:
                        os.killpg(process.pid, signal.SIGTERM)
                        process.wait(timeout=10)
                for handle in handles:
                    try:
                        signal.pidfd_send_signal(handle, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    os.close(handle)


if __name__ == '__main__':
    main()
