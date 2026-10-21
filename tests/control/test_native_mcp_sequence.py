"""Public MCP sequence playback, looping replacement, exact pixels and owned cleanup."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from PIL import Image
from test_native_quad import require, wait_file
from simxr_control import Client, ControlError

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'mcp-server'))
from mcp_application import McpApplication


def call(application, name, **arguments):
    return json.loads(application.call(name, arguments)[0].text)


def main(probe):
    with tempfile.TemporaryDirectory(prefix='sx-mseq-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_CANONICAL_SEQUENCE='1', SIMXR_PROBE_FRAMES='8',
                           SIMXR_WIDTH='64', SIMXR_HEIGHT='48')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([probe], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        application = None
        previous_endpoint = os.environ.get('SIMXR_CONTROL_SOCKET')
        try:
            wait_file(data / 'control.ready', host)
            endpoint = data / f'control.{host.pid}.sock'
            os.environ['SIMXR_CONTROL_SOCKET'] = str(endpoint)
            application = McpApplication(data)
            with Client(endpoint) as baseline:
                initial_session, = baseline.sessions()
                head = baseline.simulation_status(initial_session)['document']['configuration']['head']
                require(head['orientation'] == [0, 0, 0, 1], head)
            edit = call(application, 'set_head_pose', **dict(zip(('x', 'y', 'z'), head['position'])))
            require(edit['ok'], edit)
            program = data / 'program.jsonl'
            steps = [{'frame': frame, 'head': {'x': .3 + .1 * frame}, 'capture': frame < 2}
                     for frame in range(4)]
            steps[0]['head'].update(y=1.8, z=-.4, yaw=.2, pitch=-.1, roll=.15)
            program.write_text('\n'.join(json.dumps(step) for step in steps))
            started = call(application, 'run_pose_sequence', path=str(program))
            require(started['ok'] and started['transport'] == 'pose_sequences', started)
            original = started['sequence_id']
            (data / 'control.continue').touch()
            with Client(endpoint) as observer:
                session, = observer.sessions()
                for frame in range(3, 11):
                    wait_file(data / f'recording.{frame}.ready', host)
                    status = observer.status(session)
                    require(not status['manual'] and not status['pipeline']['paused'] and
                            status['pipeline']['permits'] == 0, status)
                    if frame < 5 or frame >= 7:
                        deadline = time.monotonic() + 8
                        while True:
                            progress = call(application, 'get_pose_sequence')
                            require(progress['ok'], progress)
                            records = [r for r in progress['recent_captures'] if r['frame'] == str(frame)]
                            if records:
                                break
                            require(time.monotonic() < deadline, progress)
                            time.sleep(.01)
                        record, = records
                        require(record['phase'] == 'ready' and record['result'] == 0 and 'error' not in record, record)
                        with Image.open(record['path']) as image:
                            require(image.size == (128, 48), image.size)
                            red_or_green = (255, 0, 0, 255) if frame % 2 else (0, 255, 0, 255)
                            require(image.crop((0, 0, 64, 48)).tobytes() == bytes(red_or_green) * (64 * 48), record)
                            colors = image.crop((64, 0, 128, 48)).getcolors(3)
                            require(colors and {color for _, color in colors} == {(0, 0, 255, 255), red_or_green},
                                    (record, colors))
                    if frame == 6:
                        program.write_text('{"frame":0,"head":{"x":0.6},"capture":true}\n')
                        loop = call(application, 'run_pose_sequence', path=str(program), loop=True)
                        require(loop['ok'] and loop['sequence_id'] != original, loop)
                        replacement = call(application, 'run_pose_sequence', path=str(program), loop=True)
                        require(replacement['ok'] and replacement['sequence_id'] != loop['sequence_id'], replacement)
                        stale = call(application, 'stop_pose_sequence', sequence_id=loop['sequence_id'])
                        require(not stale['ok'], stale)
                    if frame < 10:
                        (data / f'recording.{frame}.continue').touch()
                stopped = call(application, 'stop_pose_sequence', sequence_id=replacement['sequence_id'])
                require(stopped['ok'] and not stopped['status']['active'] and
                        stopped['status']['result'] == 'cancelled' and stopped['pending_captures'] == 0, stopped)
                require([r['frame'] for r in stopped['recent_captures']] == ['3', '4', '7', '8', '9', '10'], stopped)
                manifest = Path(stopped['directory']) / 'manifest.json'
                require(json.loads(manifest.read_text())['recent_captures'] == stopped['recent_captures'], stopped)
                require(not (data / 'sequence_command.json').exists() and not (data / 'sequence.jsonl').exists(),
                        'Public sequence adapter fell back to files')
                queued = call(application, 'run_pose_sequence', path=str(program), loop=True)
                require(queued['ok'], queued)
                foreign = observer.upload_sequence(session, b'')
                try:
                    observer.start_sequence(session, foreign)
                    raise AssertionError('Another source replaced live sequence ownership')
                except ControlError as error:
                    require(error.code.name == 'BUSY', error)
                worker = application._legacy._sequence._worker
                application.close()
                require(not worker.is_alive(), 'Sequence worker outlived its MCP owner')
                application = None
                deadline = time.monotonic() + 3
                while True:
                    try:
                        observer.start_sequence(session, foreign)
                        break
                    except ControlError as error:
                        require(error.code.name == 'BUSY' and time.monotonic() < deadline, error)
                        time.sleep(.005)
                (data / 'recording.10.continue').touch()
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
                print('PASS: public MCP real-time sequence, looping replacement/stop, exact stereo PNGs and joined cleanup')
        finally:
            if application is not None:
                application.close()
            if previous_endpoint is None:
                os.environ.pop('SIMXR_CONTROL_SOCKET', None)
            else:
                os.environ['SIMXR_CONTROL_SOCKET'] = previous_endpoint
            if host.poll() is None:
                host.kill()
                host.wait()
            remaining = host.stdout.read() if not host.stdout.closed else ''
            if remaining:
                print(remaining, file=sys.stderr)
            host.stdout.close()


if __name__ == '__main__':
    main(sys.argv[1])
