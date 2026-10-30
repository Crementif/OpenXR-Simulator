"""Shared native-control integration support."""

from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))


def probe_environment(data, **settings):
    environment = dict(os.environ, SIMXR_DATA_DIR=str(data), **settings)
    return environment


@contextmanager
def running_probe(command, environment):
    host = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        yield host
    finally:
        if host.poll() is None:
            host.kill()
        if not host.stdout.closed:
            output = host.communicate(timeout=10)[0]
            if output:
                print(output, file=sys.stderr)
        else:
            host.wait(timeout=10)


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


def command(client, lease, kind, **values):
    result = client.wait(lease.session, client.submit(lease, kind, **values)['operation_id'])
    require(result['phase'] == 'complete', result)
    client.acknowledge(lease, result['command_id'])
    return result
