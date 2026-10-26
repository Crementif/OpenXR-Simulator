"""Retained production Vulkan images outlive the application's XR/device graph."""
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Capability, Client, CommandKind, ControlError, ErrorCode, TargetKind


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def wait_file(path, process):
    deadline = time.monotonic() + 15
    while not path.exists():
        if process.poll() is not None:
            raise RuntimeError(process.communicate()[0])
        require(time.monotonic() < deadline, f'Native boundary missing: {path.name}')
        time.sleep(0.005)


def wait_ready(process):
    output = ''
    deadline = time.monotonic() + 10
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            if selector.select(0.1):
                line = process.stdout.readline()
                output += line
                if line.startswith('READY image='):
                    return output
                require(line or process.poll() is None, output)
    raise AssertionError(f'Native image reader did not become ready: {output}')


def main():
    consumers = []
    with tempfile.TemporaryDirectory(prefix='simxr-native-images-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            wait_file(data / 'control.ready', host)
            path = data / f'control.{host.pid}.sock'
            with Client(path) as client, Client(path) as observer:
                session, = client.sessions()
                needed = Capability.COMPOSED_CAPTURE | Capability.GPU_PREVIEW | Capability.IMAGE_EXPORT
                if client.status(session)['capabilities'] & needed != needed:
                    (data / 'control.continue').touch()
                    (data / 'control.retained.continue').touch()
                    output = host.communicate(timeout=15)[0]
                    require(host.returncode == 0, output)
                    print('SKIP: owned capture with native FD/fence sharing unavailable')
                    return 77
                lease = client.acquire(session)
                pause = client.submit(lease, CommandKind.PAUSE)['operation_id']
                require(client.wait(session, pause)['progress'] == 'paused', 'Pause failed')
                captures = []
                while True:
                    first = client.submit(lease, CommandKind.STEP)['operation_id']
                    record = client.operation(session, first)
                    while record['phase'] == 'queued':
                        time.sleep(0.005)
                        record = client.operation(session, first)
                    if record['phase'] == 'pending':
                        break
                    require(record['phase'] == 'complete' and record['progress'] == 'clear', record)
                    captures.append(record['capture_id'])
                (data / 'control.continue').touch()
                exact = {}
                commands = [(first, 3, TargetKind.COMPOSED_EYES),
                            (None, 4, TargetKind.COMPOSED_EYES),
                            (None, 5, TargetKind.LAYER_VIEW),
                            (None, 5, TargetKind.COMPOSED_EYES),
                            (None, 6, TargetKind.COMPOSED_EYES)]
                for operation, frame, target in commands:
                    client.renew(lease)
                    if operation is None:
                        operation = client.submit(lease, CommandKind.STEP, target=target, layer=0, view=0)['operation_id']
                    record = client.wait(session, operation)
                    require(record['phase'] == 'complete' and record['progress'] == 'snapshot_ready', record)
                    require(record['frame']['frame_id'] == frame, 'Exact native frame substituted')
                    capture = client.capture(session, record['capture_id'])
                    require(capture['phase'] == 'ready' and capture['image_available'], capture)
                    require(capture['target']['capture_kind'] == target, 'Exact target substituted')
                    captures.append(record['capture_id'])
                    exact[frame, target] = record['capture_id']
                selected = client.retain_image(session, exact[5, TargetKind.COMPOSED_EYES],
                                               target=TargetKind.LAYER_VIEW, layer=0, view=0)['image_reader_id']
                selected_info = client.image_info(selected)
                require(selected_info['capture_id'] == exact[5, TargetKind.LAYER_VIEW] and
                        selected_info['frame']['frame_id'] == 5 and
                        selected_info['target']['capture_kind'] == TargetKind.LAYER_VIEW,
                        'Same-frame source selector changed capture/frame/target identity')
                client.release_image(selected)
                for index, (frame, target, pixels) in enumerate([
                        (3, TargetKind.COMPOSED_EYES, False), (5, TargetKind.LAYER_VIEW, True)]):
                    gate = data / f'consumer-{index}.continue'
                    arguments = [sys.argv[2], str(path), str(session.id), str(session.generation),
                                 str(exact[frame, target]), str(gate)]
                    if pixels:
                        arguments.append('--verify-pixels')
                    consumer = subprocess.Popen(arguments, env=environment, stdout=subprocess.PIPE,
                                                stderr=subprocess.STDOUT, text=True)
                    consumers.append((consumer, gate, wait_ready(consumer)))
                reader = client.retain_image(session, exact[5, TargetKind.LAYER_VIEW])['image_reader_id']
                other_reader = observer.retain_image(session, exact[5, TargetKind.LAYER_VIEW])['image_reader_id']
                require(client.status(session)['pipeline']['permits'] == 0, 'Inspection admitted hidden XR work')
                for capture in captures:
                    operation = client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id']
                    require(client.wait(session, operation)['progress'] == 'released', 'Capture release failed')
                # Drain older presentation readers without permitting another application frame.
                for _ in range(16):
                    operation = client.submit(lease, CommandKind.DISPLAY_STEP)['operation_id']
                    displayed = client.wait(session, operation)
                    require(displayed['phase'] == 'complete', displayed)
                    image = client.capture(session, displayed['capture_id'])
                    released = client.submit(lease, CommandKind.RELEASE,
                                             capture=displayed['capture_id'])['operation_id']
                    require(client.wait(session, released)['progress'] == 'released', 'Display release failed')
                    client.acknowledge(lease, displayed['command_id'])
                    if (image.get('frame') or {}).get('frame_id') == 6:
                        break
                else:
                    raise AssertionError('Paused display never selected the latest retained frame')
                deadline = time.monotonic() + 3
                while True:
                    diagnostics = client.diagnostics(session, count=256)['document']
                    retired = [event for event in diagnostics['events'] if event['kind'] == 'image_retired']
                    if any(event['frame_id'] == 4 and event['detail'] == 'owned_composed' for event in retired):
                        break
                    require(time.monotonic() < deadline, 'Released private image never retired')
                    time.sleep(.005)
                require(not any(event['frame_id'] == 5 and event['detail'] == 'owned_layer_view:0:0'
                                for event in retired), 'Retained raw image retired before its readers released')
                for frame in (3, 4, 5, 6):
                    events = [event for event in diagnostics['events'] if event['frame_id'] == frame]
                    native = [event for event in events if event['kind'] == 'native_submitted']
                    ready = [event for event in events if event['kind'] == 'gpu_complete'
                             and event['detail'] == 'owned_composed']
                    require(len(native) == len(ready) == 1 and native[0]['sequence'] < ready[0]['sequence'],
                            f'Native queue acceptance/completion identity incorrect: {frame}')
                    stages = [event for event in events if event['kind'] == 'runtime_work']
                    require([event['detail'] for event in stages] ==
                            ['end_started', 'composition_prepared', 'producers_reserved',
                             'preview_started', 'preview_reserved',
                             'graphics_prepared', 'end_finished'],
                            f'Submission timing stages missing or reordered: {frame}')
                    require(all(a['host_time'] <= b['host_time'] for a, b in zip(stages, stages[1:])),
                            f'Submission timing moved backwards: {frame}')
                for (frame, target), capture_id in exact.items():
                    events = [event for event in diagnostics['events']
                              if event['kind'] == 'capture_ready' and event['related'] == capture_id]
                    require(len(events) == 1 and events[0]['frame_id'] == frame and
                            events[0]['generation'] == session.generation,
                            f'Released capture lost exact terminal history: {capture_id}')
                client.submit(lease, CommandKind.RESUME)
                wait_file(data / 'control.destroyed', host)
                require(not client.sessions(), 'Public XR session survived destruction')
                info = client.image_info(reader)
                require(info['session'] == session.id and info['generation'] == session.generation,
                        'Retained source identity changed')
                exports = [client.start_export(reader, layer=0)['export_id'] for _ in range(8)]
                try:
                    observer.start_export(other_reader, layer=0)
                except ControlError as error:
                    require(error.record['error'] == ErrorCode.CAPACITY, error.record)
                else:
                    raise AssertionError('Independent connection exceeded global native export capacity')
                export = exports[0]
                require(client.release_image(reader)['released'], 'Independent source release failed')
                deadline = time.monotonic() + 5
                status = client.export_info(export)
                while status['phase'] == 'pending':
                    require(time.monotonic() < deadline, 'Delayed export did not complete')
                    time.sleep(0.005)
                    status = client.export_info(export)
                require(status['phase'] == 'complete', status)
                pixels = bytearray()
                while len(pixels) < status['total_bytes']:
                    chunk = client.read_export(export, len(pixels), 1001)
                    require(chunk['offset'] == len(pixels) and chunk['length'] == len(chunk['bytes']), chunk)
                    require(chunk['bytes'], 'Export returned an empty intermediate chunk')
                    pixels.extend(chunk['bytes'])
                require(pixels == b'\xff\x00\x00\xff' * (64 * 48), 'Delayed exact raw pixels changed')
                require(client.release_export(export)['released'], 'Export release failed')
                for extra in exports[1:]:
                    require(client.release_export(extra)['released'], 'Additional export release failed')
                # The session worker is already gone. The next explicit request
                # must recover completed released charges using owned queues.
                deadline = time.monotonic() + 5
                while True:
                    try:
                        recovered = observer.start_export(other_reader, layer=0)['export_id']
                        break
                    except ControlError as error:
                        require(error.record['error'] == ErrorCode.CAPACITY and time.monotonic() < deadline,
                                error.record)
                        time.sleep(0.005)
                observer.release_export(recovered)
                observer.release_image(other_reader)
                for consumer, gate, prefix in consumers:
                    gate.touch()
                    output = prefix + consumer.communicate(timeout=15)[0]
                    require(consumer.returncode == 0, output)
                    require('applicationDeviceDestroyed=yes' in output and 'validationErrors=0' in output, output)
                    require('VUID-' not in output and 'Vulkan validation error' not in output, output)
                    print(output)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0, output)
                require('VUID-' not in output and 'Vulkan validation error' not in output, output)
                require('preserving native owners' not in output and 'preserving bounded native owners' not in output,
                        output)
                print(output)
            require(not path.exists(), 'Production endpoint survived process teardown')
            print('PASS: independent native FD consumers and SDK export after application XR/Vulkan destruction')
            return 0
        finally:
            for process in [host] + [item[0] for item in consumers]:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if process.stdout:
                    process.stdout.close()


if __name__ == '__main__':
    raise SystemExit(main())
