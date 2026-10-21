"""Python controls the actual OpenXR loader probe through the production socket."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Client, Capability, CommandKind, TargetKind


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    with tempfile.TemporaryDirectory(prefix='simxr-native-control-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        process = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 10
            while not (data / 'control.ready').exists():
                if process.poll() is not None:
                    raise RuntimeError(process.communicate()[0])
                require(time.monotonic() < deadline, 'Native loader never reached control boundary')
                time.sleep(0.005)
            path = data / f'control.{process.pid}.sock'
            with Client(path) as client, Client(path) as observer:
                session, = client.sessions()
                status = client.status(session)
                if not status['capabilities'] & Capability.COMPOSED_CAPTURE:
                    (data / 'control.continue').touch()
                    output = process.communicate(timeout=10)[0]
                    require(process.returncode == 0, output)
                    print('SKIP: native owned composed capture capability unavailable')
                    return 77
                require(status['capabilities'] & Capability.LAYER_VIEW_CAPTURE, 'Native raw capability absent')
                lease = client.acquire(session)
                pause = client.submit(lease, CommandKind.PAUSE)['operation_id']
                require(client.wait(session, pause)['progress'] == 'paused', 'Native pause did not settle')
                require(observer.status(session)['manual'], 'Observer could not inspect paused native session')
                retained = []
                # Initial loading clears may already be retained; inspect them without issuing more work.
                while True:
                    first = client.submit(lease, CommandKind.STEP)['operation_id']
                    record = client.operation(session, first)
                    while record['phase'] == 'queued':
                        time.sleep(0.005)
                        record = client.operation(session, first)
                    if record['phase'] in ('queued', 'pending'):
                        break
                    require(record['phase'] == 'complete' and record['progress'] == 'clear',
                            'Unexpected pre-boundary native result')
                    inspected = client.inspect(session, record['capture_id'])
                    require(inspected['document']['frame_id'] == record['frame']['frame_id'], 'Historical clear identity changed')
                    require(inspected['document']['submission']['output_disposition'] == 'clear', 'Historical clear disposition changed')
                    require(client.release_inspection(inspected['inspection_id'])['released'], 'Clear inspection reader release failed')
                    retained.append(record['capture_id'])
                transactions = {}
                for frame, x in ((3, .31), (4, -.27)):
                    queued = client.schedule(session, [
                        {'kind': 'take_control', 'channel': 'gaze'},
                        {'kind': 'gaze', 'yaw': x, 'pitch': 0, 'available': True, 'tracked': True}], frame=frame)
                    transactions[frame] = queued['transaction_id']
                historical_commands = []
                (data / 'control.continue').touch()
                commands = [(first, 3, TargetKind.COMPOSED_EYES),
                            (None, 4, TargetKind.COMPOSED_EYES),
                            (None, 5, TargetKind.LAYER_VIEW),
                            (None, 5, TargetKind.COMPOSED_EYES),
                            (None, 6, TargetKind.COMPOSED_EYES)]
                for operation, frame, target in commands:
                    client.renew(lease)
                    if operation is None:
                        operation = client.submit(lease, CommandKind.STEP, target=target, layer=0, view=0)['operation_id']
                    record = client.wait(session, operation, timeout=5)
                    require(record['phase'] == 'complete' and record['progress'] == 'snapshot_ready', record)
                    require(record['frame']['frame_id'] == frame, 'Step substituted another native frame')
                    capture = client.capture(session, record['capture_id'])
                    require(capture['phase'] == 'ready' and capture['image_available'], 'Native snapshot not ready')
                    require(capture['frame']['frame_id'] == frame, 'Snapshot metadata identity changed')
                    require(capture['target']['capture_kind'] == target, 'Native target identity changed')
                    require(capture['frame']['submission']['output_disposition'] == 'submitted', 'Exact capture dropped')
                    require(capture['frame']['submission']['layer_count'] == 2, 'Layer metadata missing')
                    inspected = client.inspect(session, record['capture_id'])
                    state = inspected['document']
                    require(state['frame_id'] == frame and state['generation'] == session.generation, 'Historical inspection substituted identity')
                    require(len(state['layers']['layers']) == 2 and state['layers']['layers'][0]['kind'] == 'projection', 'Historical ordered layers missing')
                    require(len(state['layers']['layers'][0]['views']) == 2, 'Historical projection views missing')
                    require(state['tracking'] is not None and state['input']['revision'] >= 0, 'Historical CPU input/tracking missing')
                    if frame in transactions:
                        applied = client.wait_simulation(session, transactions[frame])
                        require(applied['phase'] == 'applied', applied)
                        expected = .31 if frame == 3 else -.27
                        require(abs(state['tracking']['input']['gaze']['yaw'] - expected) < .0001,
                                'Captured gaze input was replaced with another frame')
                        command_revision = state['applied_commands']['revision_id']
                        command_state = client.revision(command_revision)['document']
                        require(command_state['frame_id'] == frame and command_state['configuration_available'],
                                'Applied-command revision lost its frame/configuration')
                        require(abs(command_state['configuration']['gaze']['yaw'] - expected) < .0001,
                                'Historical configuration differs from applied gaze')
                        require(state['availability']['native_driver_validation'] == 'not_recorded' and
                                state['availability']['fb_foveation'] == 'unsupported',
                                'Inspection falsely claimed unavailable evidence')
                        historical_commands.append((inspected['inspection_id'], command_revision, command_state))
                    view = client.revision(state['view_settings']['revision_id'])
                    require(view['revision_kind'] == 'view_settings_v1', 'Historical view revision missing')
                    if frame not in transactions:
                        require(client.release_inspection(inspected['inspection_id'])['released'], 'Inspection reader release failed')
                    retained.append(record['capture_id'])
                for inspection, revision, original in historical_commands:
                    require(client.revision(revision)['document'] == original,
                            'Later submissions rewrote the retained applied-command configuration')
                    client.release_inspection(inspection)
                require(client.status(session)['pipeline']['permits'] == 0, 'Inspection granted hidden admission')
                for capture in retained:
                    operation = client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id']
                    require(client.wait(session, operation)['progress'] == 'released', 'Native Release failed')
                resumed = client.submit(lease, CommandKind.RESUME)['operation_id']
                # Host may destroy its public session immediately after resume; admission is the ack.
                require(resumed > 0, 'Resume did not return an operation ID')
                output = process.communicate(timeout=10)[0]
                require(process.returncode == 0, output)
                print(output)
            require(not path.exists(), 'Native endpoint path survived process teardown')
            print('PASS: Python -> production native control; exact composed/raw frames; owned fences; release/resume')
            return 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if process.stdout:
                remaining = process.stdout.read() if not process.stdout.closed else ""
                if remaining:
                    print(remaining, file=sys.stderr)
                process.stdout.close()


if __name__ == '__main__':
    raise SystemExit(main())
