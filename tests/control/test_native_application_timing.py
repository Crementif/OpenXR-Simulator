"""Application GPU timing survives paused stepping and rejected frame submissions."""
from pathlib import Path
import sys
import tempfile
import time

from native_support import command, probe_environment, require, running_probe, wait_file
from simxr_control import Client, CommandKind


def completed_timing(client, session, frame):
    deadline = time.monotonic() + 5
    while True:
        records = client.frame_history(session, after=frame - 1, count=1)['frames']
        require(len(records) == 1 and records[0]['frame_id'] == frame, 'Wrong historical frame')
        record = records[0]
        if record.get('application_gpu_time', -1) >= 0:
            return record['application_gpu_time']
        require(time.monotonic() < deadline, f'Application timing never completed for paused frame {frame}')
        time.sleep(.005)


def main():
    with tempfile.TemporaryDirectory(prefix='simxr-application-timing-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1', SIMXR_PROBE_FRAMES='4',
                                        SIMXR_PROBE_QUAD='1' if '--quad' in sys.argv else '0')
        with running_probe([sys.argv[1]], environment) as host:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                lease = client.acquire(session)
                command(client, lease, CommandKind.PAUSE)
                # Drain any already submitted clear frame before the probe's first rendered frame.
                while True:
                    operation = client.submit(lease, CommandKind.STEP)['operation_id']
                    record = client.operation(session, operation)
                    deadline = time.monotonic() + 5
                    while record['phase'] == 'queued':
                        require(time.monotonic() < deadline, 'Step never left the queue')
                        time.sleep(.005)
                        record = client.operation(session, operation)
                    if record['phase'] == 'pending':
                        break
                    require(record['phase'] == 'complete' and record['progress'] == 'clear', record)
                    command(client, lease, CommandKind.RELEASE, capture=record['capture_id'])
                (data / 'control.continue').touch()
                timings = {}
                for frame in (3, 4, 5, 6):
                    if frame != 3:
                        operation = client.submit(lease, CommandKind.STEP)['operation_id']
                    record = client.wait(session, operation)
                    require(record['progress'] == 'snapshot_ready' and record['frame']['frame_id'] == frame, record)
                    # Frame 5 delays GPU completion until after EndFrame returns. In quad mode it
                    # also rejects an invalid layer first, then retries that same begun frame.
                    timings[frame] = completed_timing(client, session, frame)
                    require(client.status(session)['pipeline']['permits'] == 0, 'Timing collection advanced a frame')
                    command(client, lease, CommandKind.RELEASE, capture=record['capture_id'])
                for frame, duration in timings.items():
                    require(completed_timing(client, session, frame) == duration, 'Later frame rewrote timing')
                client.submit(lease, CommandKind.RESUME)
                output = host.communicate(timeout=10)[0]
                require(host.returncode == 0, output)
                print('PASS: application timing completes while paused and survives submission retry')


if __name__ == '__main__':
    main()
