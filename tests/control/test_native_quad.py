"""Explicit GPU export analysis of the real Linux VARJO quad-view path."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Capability, Client, CommandKind, TargetKind


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def wait_file(path, host):
    deadline = time.monotonic() + 15
    while not path.exists():
        if host.poll() is not None:
            raise AssertionError(host.communicate()[0])
        require(time.monotonic() < deadline, f'Native boundary missing: {path.name}')
        time.sleep(0.005)


def export(client, reader, layer):
    identity = client.start_export(reader, layer=layer)['export_id']
    try:
        deadline = time.monotonic() + 5
        status = client.export_info(identity)
        while status['phase'] == 'pending':
            require(time.monotonic() < deadline, 'GPU export did not complete')
            time.sleep(0.005)
            status = client.export_info(identity)
        require(status['phase'] == 'complete', status)
        pixels = bytearray()
        while len(pixels) < status['total_bytes']:
            chunk = client.read_export(identity, len(pixels), 64001)
            require(chunk['offset'] == len(pixels) and chunk['bytes'], 'Invalid export chunk')
            pixels.extend(chunk['bytes'])
        return status, pixels
    finally:
        client.release_export(identity)


def main():
    foveated = "--foveated" in sys.argv[2:]
    with tempfile.TemporaryDirectory(prefix='simxr-native-quad-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_QUAD='1')
        if foveated:
            environment.update(SIMXR_PROBE_FOVEATED='1', SIMXR_PROBE_FRAMES='4')
        else:
            environment.pop('SIMXR_PROBE_FOVEATED', None)
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1], '--quad'], env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                needed = Capability.COMPOSED_CAPTURE | Capability.LAYER_VIEW_CAPTURE | Capability.IMAGE_EXPORT
                if client.status(session)['capabilities'] & needed != needed:
                    (data / 'control.continue').touch()
                    (data / 'control.retained.continue').touch()
                    output = host.communicate(timeout=15)[0]
                    require(host.returncode == 0, output)
                    print('SKIP: owned composed/raw capture and explicit export unavailable')
                    return 77
                lease = client.acquire(session)
                pause = client.submit(lease, CommandKind.PAUSE)['operation_id']
                require(client.wait(session, pause)['progress'] == 'paused', 'Pause failed')
                captures = []
                while True:
                    pending = client.submit(lease, CommandKind.STEP)['operation_id']
                    record = client.operation(session, pending)
                    while record['phase'] == 'queued':
                        time.sleep(0.005)
                        record = client.operation(session, pending)
                    if record['phase'] == 'pending':
                        break
                    require(record['phase'] == 'complete' and record['progress'] == 'clear', record)
                    captures.append(record['capture_id'])
                readers = []
                inspections = []
                commands = [(pending, 3, TargetKind.COMPOSED_EYES, 0),
                            (None, 4, TargetKind.LAYER_VIEW, 2),
                            (None, 5, TargetKind.LAYER_VIEW, 3),
                            (None, 6, TargetKind.LAYER_VIEW, 0)]
                if foveated:
                    if '--raw-views' in sys.argv[2:]:
                        commands[-1] = (None, 6, TargetKind.LAYER_VIEW, 1)
                    else:
                        commands = [(pending, 3, TargetKind.COMPOSED_EYES, 0)]
                        commands.extend((None, frame, TargetKind.COMPOSED_EYES, 0) for frame in range(4, 7))
                for operation, frame, target, view in commands:
                    client.renew(lease)
                    transaction = None
                    if foveated:
                        queued = client.schedule(session, [
                            {'kind': 'take_control', 'channel': 'gaze'},
                            {'kind': 'gaze', 'yaw': .55 if frame == 3 else -.55,
                             'available': frame != 6, 'tracked': True}], frame=frame)
                        require(queued['phase'] == 'queued', queued)
                        transaction = queued['transaction_id']
                    if frame == 3:
                        (data / 'control.continue').touch()
                    if operation is None:
                        operation = client.submit(lease, CommandKind.STEP, target=target, layer=0, view=view)['operation_id']
                    record = client.wait(session, operation)
                    require(record['phase'] == 'complete' and record['progress'] == 'snapshot_ready', record)
                    require(record['frame']['frame_id'] == frame, 'Quad inspection substituted another frame')
                    if transaction:
                        require(client.wait_simulation(session, transaction)['phase'] == 'applied',
                                'Gaze transaction missed the inspected frame')
                    capture = client.capture(session, record['capture_id'])
                    require(capture['target']['capture_kind'] == target and capture['image_available'], capture)
                    if target == TargetKind.LAYER_VIEW:
                        require(capture['target']['view'] == view and capture['target']['layer'] == 0, capture)
                    captures.append(record['capture_id'])
                    inspected = client.inspect(session, record['capture_id'])
                    historical = inspected['document']
                    milestones = historical['frame_trace']['events']
                    require(any(event['kind'] == 'native_submitted' and event['frame_id'] == frame
                                for event in milestones), 'Immutable trace omitted native acceptance')
                    if frame == 5:
                        require(any(event['kind'] == 'validation' and event['detail'] == 'submitted_layers'
                                    and event['frame_id'] == frame and event['result'] < 0
                                    for event in milestones), 'Rejected layer lost its exact frame validation event')
                    projection = historical['layers']['layers'][0]
                    require(projection['kind'] == 'projection' and len(projection['views']) == 4,
                            'Historical four-view projection metadata missing')
                    require([item['color']['array_slice'] for item in projection['views']] == list(range(4)),
                            'Historical outer/inset array order changed')
                    query = historical['view_query']
                    require(query and query['display_time'] == historical['submission']['display_time'],
                            'Missing timestamp-matched view query')
                    require(query['foveated_requested'] == (foveated and frame != 4) and
                            query['gaze_active'] == (foveated and frame not in (4, 6)),
                            'Historical foveation request/availability substituted current gaze')
                    for index, view_data in enumerate(projection['views']):
                        require(query['fovs'][index] == view_data['fov'], 'Query/submitted FOV disagreement')
                    if foveated and frame in (3, 5):
                        center = (query['fovs'][2]['left'] + query['fovs'][2]['right']) / 2
                        require(center < -.05 if frame == 3 else center > .05, 'Gaze did not move the inset')
                    revision_id = historical['view_settings']['revision_id']
                    optics = client.revision(revision_id)['document']
                    require(optics['view_layout'] == 'stereo_with_inset' and optics['active_view_count'] == 4,
                            'Historical selected view configuration missing')
                    inspections.append((inspected['inspection_id'], revision_id))
                    reader = client.retain_image(session, record['capture_id'])['image_reader_id']
                    readers.append((reader, target, view, frame))
                require(client.status(session)['pipeline']['permits'] == 0, 'Quad capture admitted hidden work')
                for capture in captures:
                    released = client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id']
                    require(client.wait(session, released)['progress'] == 'released', 'Capture release failed')
                resumed = client.submit(lease, CommandKind.RESUME)['operation_id']
                require(client.wait(session, resumed)['phase'] == 'complete', 'Resume failed')
                wait_file(data / 'control.destroyed', host)
                require(not client.sessions(), 'Public XR session survived destruction')
                palette = {0: bytes((255, 0, 0, 255)), 1: bytes((0, 0, 255, 255)), 2: bytes((0, 255, 0, 255)), 3: bytes((255, 255, 0, 255))}
                for inspection, revision in inspections:
                    require(client.revision(revision)['document']['active_view_count'] == 4,
                            'Retained quad optics changed after session destruction')
                    client.release_inspection(inspection)
                for reader, target, view, frame in readers:
                    info = client.image_info(reader)
                    require(info['session'] == session.id and info['generation'] == session.generation,
                            'Original source identity changed after app-device destruction')
                    require(info['frame']['frame_id'] == frame, 'Retained per-view frame identity changed')
                    if target == TargetKind.LAYER_VIEW:
                        status, pixels = export(client, reader, 0)
                        require(pixels == palette[view] * (64 * 48), f'Raw view {view} source bytes changed')
                    else:
                        for eye, inset, outer in [(0, palette[2], palette[0]),
                                                  (1, palette[3], bytes((0, 0, 255, 255)))]:
                            status, pixels = export(client, reader, eye)
                            width, height = info['image_descriptor']['width'], info['image_descriptor']['height']
                            center = ((height // 2) * width + width // 2) * 4
                            if foveated and frame in (3, 5):
                                shifted_x = width // 8 if frame == 3 else 7 * width // 8
                                shifted = ((height // 2) * width + shifted_x) * 4
                                require(pixels[center:center + 4] == outer, f'Eye {eye} retained a centered inset')
                                require(pixels[shifted:shifted + 4] == inset, f'Eye {eye} missing gaze-steered inset')
                            else:
                                require(pixels[center:center + 4] == inset, f'Eye {eye} missing fixed inset')
                            require(pixels[:4] == outer, f'Eye {eye} lost outer image')
                    client.release_image(reader)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0, output)
                require('VUID-' not in output and 'Vulkan validation error' not in output, output)
                require('preserving native owners' not in output, output)
                print(output)
            print('PASS: actual four-view composition and exact selected inset/outer RGBA after app-device destruction')
            return 0
        finally:
            if host.poll() is None:
                host.kill()
                host.wait()
            remaining = host.stdout.read() if not host.stdout.closed else ""
            if remaining:
                print(remaining, file=sys.stderr)
            host.stdout.close()


if __name__ == '__main__':
    raise SystemExit(main())
