#!/usr/bin/env python3
"""Exercise the installed Python SDK from outside its source checkout."""

import argparse
from importlib import metadata
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

import simxr_control
from simxr_control import Client, encode_command, decode_command


def exercise(probe):
    location = Path(simxr_control.__file__).resolve()
    assert location.is_relative_to(Path(sys.prefix).resolve()), location
    assert metadata.version('simxr-control').split('.')[0] == '2'
    assert not metadata.requires('simxr-control')
    encoded = encode_command({'kind': 'controller_controls', 'hand': 1, 'trigger': .75, 'primary': True})
    decoded = decode_command({'encoding': 'simxr-command-v1-hex', 'data': encoded.value.hex()})
    assert decoded['hand'] == 1 and decoded['trigger'] == .75 and decoded['primary'] is True
    assert encode_command(decoded).value == encoded.value
    with tempfile.TemporaryDirectory(prefix='simxr-sdk-installed-') as directory:
        env = dict(os.environ, SIMXR_DATA_DIR=directory)
        env.pop('PYTHONPATH', None)
        env.pop('SIMXR_CONTROL_SOCKET', None)
        try:
            Client(Path(directory) / 'missing.sock')
        except (OSError, simxr_control.ProtocolError):
            pass
        else:
            raise AssertionError('A missing endpoint unexpectedly connected')
        if probe:
            child = subprocess.Popen([str(probe)], env=dict(env, SIMXR_PROBE_CONTROL='1'),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                ready = Path(directory) / 'control.ready'
                deadline = time.monotonic() + 15
                while not ready.exists():
                    assert child.poll() is None, child.communicate()[0]
                    assert time.monotonic() < deadline, 'Probe did not publish its endpoint'
                    time.sleep(.05)
                with Client(Path(directory) / f'control.{child.pid}.sock') as client:
                    session, = client.sessions()
                    assert client.status(session)['running']
            finally:
                (Path(directory) / 'control.continue').touch()
                try:
                    output = child.communicate(timeout=10)[0]
                except subprocess.TimeoutExpired:
                    child.kill()
                    output = child.communicate()[0]
                    raise AssertionError(f'Probe did not exit: {output!r}')
                assert child.returncode == 0, output.decode(errors='replace')
    print('PASS installed SDK imports, command codec and transport')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, help='Linux XR probe to connect to using XR_RUNTIME_JSON')
    exercise(parser.parse_args().probe)
