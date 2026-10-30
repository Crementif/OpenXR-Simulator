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

from native_support import require, wait_file

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Capability, Client


def check_multiple_gpu_tabs(probe, interaction, count=2, input_test=False, portable=False, budget_test=False,
                            replay_close=False):
    hosts = []
    with tempfile.TemporaryDirectory(prefix='simxr-multi-gpu-') as temporary:
        root = Path(temporary)
        try:
            for index in range(count):
                data = root / str(index)
                data.mkdir()
                environment = dict(os.environ, SIMXR_DATA_DIR=str(data), SIMXR_PROBE_CONTROL='1',
                                   SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                                   SIMXR_PROBE_RENDER_GATE='1')
                if input_test:
                    environment.update(SIMXR_PROBE_FRAMES='12', SIMXR_PROBE_INTERACTIVE='1')
                host = subprocess.Popen([probe], env=environment, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
                hosts.append((host, data))
                wait_file(data / 'control.ready', host)
            for host, data in hosts:
                (data / 'control.continue').touch()
                wait_file(data / 'rendering.3.ready', host)
            endpoints = [data / f'control.{host.pid}.sock' for host, data in hosts]
            for endpoint in endpoints:
                with Client(endpoint) as observer:
                    session, = observer.sessions()
                    needed = Capability.GPU_PREVIEW | Capability.LIVE_PREVIEW
                    if observer.status(session)['capabilities'] & needed != needed:
                        print('SKIP: production owned GPU preview capability unavailable')
                        return 77
            environment = dict(os.environ, SIMXR_TABS_GPU_FIRST=str(endpoints[0]),
                               SIMXR_TABS_GPU_SECOND=str(endpoints[1]),
                               SIMXR_COMPANION_SETTINGS=str(root / 'settings.json'), SIMXR_DATA_DIR=str(root))
            if portable:
                environment.pop('SIMXR_TABS_GPU_FIRST')
                environment.pop('SIMXR_TABS_GPU_SECOND')
                environment.update(SIMXR_TABS_LIVE_FIRST=str(endpoints[0]), SIMXR_TABS_LIVE_SECOND=str(endpoints[1]),
                                   SIMXR_TABS_LIVE_DIRECTORY=str(root), SIMXR_TABS_LIVE_GATE='1')
                if budget_test:
                    environment['SIMXR_TABS_BUDGET_QA'] = '1'
            if input_test:
                environment['SIMXR_TABS_INPUT_QA'] = '1'
            if replay_close:
                environment['SIMXR_TABS_REPLAY_CLOSE_QA'] = '1'
            if count == 4:
                prefix = 'SIMXR_TABS_LIVE_' if portable else 'SIMXR_TABS_GPU_'
                environment.update({prefix + 'THIRD': str(endpoints[2]), prefix + 'FOURTH': str(endpoints[3])})
            result = subprocess.run([interaction], env=environment, capture_output=True, text=True,
                                    timeout=35 if budget_test else 30 if input_test else 18)
            output = result.stdout + result.stderr
            require(result.returncode == 0, output)
            marker = ('PASS shared tab snapshot budget' if budget_test else 'PASS portable live tabs' if portable else
                      'PASS tab gamepad input ownership' if input_test else 'PASS native GPU tabs')
            require(marker in output and output.count('normalReadback=0') >= count, output)
            if replay_close:
                require('PASS background replay tab close' in output, output)
            require('VUID-' not in output and 'preserving' not in output, output)
            print(output)
            for host, data in hosts:
                for gate in range(6, 15 if input_test else 7):
                    (data / f'rendering.{gate}.continue').touch()
            for host, data in hosts:
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
                host_output = host.communicate(timeout=10)[0]
                require(host.returncode == 0 and 'VUID-' not in host_output and 'preserving' not in host_output,
                        host_output)
            return 0
        except BaseException:
            for host, data in hosts:
                if host.poll() is not None:
                    print(f"Probe {data.name} exited {host.returncode}:\n{host.communicate()[0]}", flush=True)
                else:
                    try:
                        with Client(data / f'control.{host.pid}.sock') as observer:
                            session, = observer.sessions()
                            state = observer.simulation_status(session)['document']
                            recent = [(item['phase'], item.get('target_frame'), item.get('applied_frame'))
                                      for item in state.get('transactions', [])[-4:]]
                            print(f"Probe {data.name}: frame={state.get('frame_id')} pending={state.get('pending')} "
                                  f"recent (phase, target, applied)={recent}", flush=True)
                    except Exception as error:
                        print(f"Probe state unavailable: {error}", flush=True)
            raise
        finally:
            for host, _ in hosts:
                if host.poll() is None:
                    host.kill()
                    host.wait()
                if host.stdout:
                    host.stdout.close()


def check_arguments(companion):
    environment = dict(os.environ, SDL_VIDEODRIVER='simxr-unavailable')
    for arguments, option in [(['--connect', '--runtime-smoke'], '--connect'),
                              (['--screenshot', ''], '--screenshot'),
                              (['--export', '--hidden'], '--export'),
                              (['--smoke-frames'], '--smoke-frames'),
                              (['--proof-consumer', '-1'], '--proof-consumer')]:
        result = subprocess.run([companion, *arguments], env=environment, capture_output=True,
                                text=True, timeout=5)
        output = result.stdout + result.stderr
        require(result.returncode == 2 and option in output and 'Companion:' not in output,
                f'Malformed option reached application startup: {arguments}\n{output}')
    result = subprocess.run([companion, '--smoke-frames', '8', '--hidden'], env=environment,
                            capture_output=True, text=True, timeout=5)
    output = result.stdout + result.stderr
    require(result.returncode == 77 and 'SDL video/gamepad initialization failed' in output,
            'Finite smoke did not classify unavailable SDL as a skip\n' + output)
    print('PASS CLI rejects malformed values before startup and classifies unavailable SDL')


def main():
    if '--multi-tab' in sys.argv[3:]:
        return check_multiple_gpu_tabs(sys.argv[1], sys.argv[2], 4 if '--four-tab' in sys.argv[3:] else 2,
                                       '--tab-input' in sys.argv[3:], '--portable-tabs' in sys.argv[3:],
                                       '--budget-tabs' in sys.argv[3:], '--replay-close' in sys.argv[3:])
    if '--check-cli' in sys.argv[3:] or '--cli-only' in sys.argv[3:]:
        check_arguments(sys.argv[2])
    if '--cli-only' in sys.argv[3:]:
        return 0
    visible = '--visible' in sys.argv[3:]
    if visible and os.getenv('SDL_VIDEODRIVER') == 'x11' and not os.getenv('DISPLAY'):
        print('SKIP: no X11 display available')
        return 77
    pixels = '--verify-pixels' in sys.argv[3:]
    failed_export = '--fail-export' in sys.argv[3:]
    require(not (pixels and failed_export), 'Choose pixel verification or the failed-export check')
    midtone = '--midtone' in sys.argv[3:]
    srgb = '--srgb' in sys.argv[3:]
    companion = None
    with tempfile.TemporaryDirectory(prefix='simxr-companion-runtime-') as temporary:
        data = Path(temporary)
        export_path = data / 'frame-é.png'
        screenshot_path = data / 'companion-é.bmp'
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1')
        environment['SIMXR_CONTROL_TRACE'] = '1'
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
                    export_path.write_bytes(b'Previous export must not count as completion')
                    arguments += ['--export', str(export_path), '--screenshot', str(screenshot_path)]
                elif failed_export:
                    export_path.mkdir()
                    arguments += ['--export', str(export_path)]
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
                if failed_export:
                    require(companion.returncode == 1 and
                            'Runtime smoke export failed:' in output and
                            'PASS companion production controls' not in output,
                            'An existing destination directory was accepted as a successful export\n' + output)
                    require(export_path.is_dir(), 'Failed export changed the destination directory')
                else:
                    require(companion.returncode == 0, output)
                    require('PASS companion production controls' in output, output)
                    require('0 validation errors' in output and 'normalReadback=0' in output, output)
                require('VUID-' not in output and 'preserving' not in output, output)
                wait_file(data / 'control.destroyed', host)
                require(not observer.sessions(), 'Public XR sessions survived teardown')
                if pixels:
                    from PIL import Image
                    with Image.open(export_path) as image:
                        require(image.format == 'PNG' and image.size == (64, 48),
                                'Companion export lost its image format or dimensions')
                        source = image.convert('RGB').tobytes()
                    expected = (137, 188, 225) if midtone and srgb else (
                        (64, 128, 191) if midtone else (255, 0, 0))
                    require(len(source) == 64 * 48 * 3 and
                            all(abs(value - expected[index % 3]) <= (1 if midtone else 0)
                                for index, value in enumerate(source)),
                            f'Explicit source export changed color encoding: {list(source[:3])}')
                    screenshot = screenshot_path.read_bytes()
                    require(screenshot[:2] == b'BM', 'Companion screenshot missing')
                    offset = struct.unpack_from('<I', screenshot, 10)[0]
                    rgba = screenshot[offset:]
                    display = (137, 188, 225) if midtone else (255, 0, 0)
                    matching = sum(all(abs(rgba[index + 2 - channel] - display[channel]) <= 2
                                       for channel in range(3)) for index in range(0, len(rgba), 4))
                    if destination := os.getenv('SIMXR_COMPANION_QA_OUTPUT'):
                        destination = Path(destination) / ('srgb' if srgb else 'unorm') if midtone else Path(destination)
                        destination.mkdir(parents=True, exist_ok=True)
                        shutil.copy(screenshot_path, destination / 'companion.bmp')
                        shutil.copy(export_path, destination / 'frame.png')
                    require(matching > 2000,
                            f'Actual companion window did not show display-encoded RGB {display}: {matching}')
                    print(f'PASS unchanged source bytes and actual display-encoded window pixels: {matching}')
            (data / 'control.retained.continue').touch()
            host_output = host_prefix + host.communicate(timeout=10)[0]
            require(host.returncode == 0, host_output)
            require('VUID-' not in host_output and 'preserving' not in host_output, host_output)
            print(host_output)
            if failed_export:
                print('PASS failed explicit export cannot qualify through an existing destination')
            else:
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
