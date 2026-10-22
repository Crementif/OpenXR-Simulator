"""Actual companion controls and GPU preview through the production endpoint."""
import os
from pathlib import Path
import selectors
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Capability, Client
from test_native_quad import wait_file


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    visible = '--visible' in sys.argv[3:]
    if visible and os.getenv('SDL_VIDEODRIVER') == 'x11' and not os.getenv('DISPLAY'):
        print('SKIP: no X11 display available')
        return 77
    pixels = '--verify-pixels' in sys.argv[3:]
    midtone = '--midtone' in sys.argv[3:]
    srgb = '--srgb' in sys.argv[3:]
    companion = None
    with tempfile.TemporaryDirectory(prefix='simxr-companion-runtime-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1')
        environment['SIMXR_CONTROL_TRACE'] = '1'
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        if midtone:
            environment['SIMXR_PROBE_MIDTONE'] = '1'
        if srgb:
            environment['SIMXR_PROBE_SRGB'] = '1'
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic() + 10
            while not (data / 'control.ready').exists():
                require(host.poll() is None, host.communicate()[0] if host.poll() is not None else '')
                require(time.monotonic() < deadline, 'XR host did not reach production control boundary')
                time.sleep(0.005)
            path = data / f'control.{host.pid}.sock'
            with Client(path) as observer:
                session, = observer.sessions()
                needed = Capability.COMPOSED_CAPTURE | Capability.GPU_PREVIEW | Capability.LIVE_PREVIEW
                if observer.status(session)['capabilities'] & needed != needed:
                    (data / 'control.continue').touch()
                    (data / 'control.retained.continue').touch()
                    output = host.communicate(timeout=10)[0]
                    require(host.returncode == 0, output)
                    print('SKIP: production owned GPU preview capability unavailable')
                    return 77
                arguments = [sys.argv[2], '--connect', str(path), '--runtime-smoke', '--validation']
                if not visible:
                    arguments.append('--hidden')
                if pixels:
                    arguments += ['--export', str(data / 'frame.ppm'), '--screenshot', str(data / 'companion.bmp')]
                companion = subprocess.Popen(arguments, env=environment, stdout=subprocess.PIPE,
                                             stderr=subprocess.STDOUT)
                output = ''
                host_prefix = ''
                continued = False
                deadline = time.monotonic() + 25
                with selectors.DefaultSelector() as selector:
                    selector.register(companion.stdout, selectors.EVENT_READ, 'companion')
                    selector.register(host.stdout, selectors.EVENT_READ, 'host')
                    while companion.poll() is None and time.monotonic() < deadline:
                        for key, _ in selector.select(0.05):
                            chunk = os.read(key.fd, 65536)
                            if not chunk:
                                selector.unregister(key.fileobj)
                                continue
                            if key.data == 'host':
                                host_prefix += chunk.decode('utf-8', errors='replace')
                                continue
                            output += chunk.decode('utf-8', errors='replace')
                            if not continued and 'READY companion production controls paused' in output:
                                require(observer.status(session)['manual'], 'Companion did not actually pause the host')
                                (data / 'control.continue').touch()
                                continued = True
                    require(companion.poll() is not None, 'Companion timed out\n' + output)
                output += companion.communicate(timeout=5)[0].decode('utf-8', errors='replace')
                if companion.returncode and host.poll() is not None:
                    output += '\nXR host output:\n' + host.communicate()[0]
                if companion.returncode:
                    output += '\nXR host observed output:\n' + host_prefix
                print(output)
                if companion.returncode == 77:
                    return 77
                require(companion.returncode == 0, output)
                require('PASS companion production controls' in output, output)
                require('0 validation errors' in output and 'normalReadback=0' in output, output)
                require('VUID-' not in output and 'preserving' not in output, output)
                wait_file(data / 'control.destroyed', host)
                require(not observer.sessions(), 'Public XR sessions survived teardown')
                if pixels:
                    ppm = (data / 'frame.ppm').read_bytes()
                    header, source = ppm.split(b'\n255\n', 1)
                    require(header.startswith(b'P6\n# session=') and b'frame=4 capture=' in header and
                            b'target=2 layer=0 view=0 slices=1 slice_height=48' in header and
                            header.endswith(b'\n64 48'),
                            'Viewable companion export lost frame identity or dimensions')
                    expected = (137, 188, 225) if midtone and srgb else (
                        (64, 128, 191) if midtone else (255, 0, 0))
                    require(len(source) == 64 * 48 * 3 and
                            all(abs(value - expected[index % 3]) <= (1 if midtone else 0)
                                for index, value in enumerate(source)),
                            f'Explicit source export changed color encoding: {list(source[:3])}')
                    screenshot = (data / 'companion.bmp').read_bytes()
                    require(screenshot[:2] == b'BM', 'Companion screenshot missing')
                    offset = struct.unpack_from('<I', screenshot, 10)[0]
                    rgba = screenshot[offset:]
                    display = (137, 188, 225) if midtone else (255, 0, 0)
                    matching = sum(all(abs(rgba[index + 2 - channel] - display[channel]) <= 2
                                       for channel in range(3)) for index in range(0, len(rgba), 4))
                    if destination := os.getenv('SIMXR_COMPANION_QA_OUTPUT'):
                        destination = Path(destination) / ('srgb' if srgb else 'unorm') if midtone else Path(destination)
                        destination.mkdir(parents=True, exist_ok=True)
                        shutil.copy(data / 'companion.bmp', destination / 'companion.bmp')
                        shutil.copy(data / 'frame.ppm', destination / 'frame.ppm')
                    require(matching > 2000,
                            f'Actual companion window did not show display-encoded RGB {display}: {matching}')
                    print(f'PASS unchanged source bytes and actual display-encoded window pixels: {matching}')
            (data / 'control.retained.continue').touch()
            host_output = host_prefix + host.communicate(timeout=10)[0]
            require(host.returncode == 0, host_output)
            require('VUID-' not in host_output and 'preserving' not in host_output, host_output)
            print(host_output)
            print('PASS production companion controls, exact composed/raw GPU images, retention after XR/Vulkan destruction')
            return 0
        finally:
            for process in (companion, host):
                if process is None:
                    continue
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if process.stdout:
                    process.stdout.close()


if __name__ == '__main__':
    raise SystemExit(main())
