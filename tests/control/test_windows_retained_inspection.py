"""Qualify owned Vulkan inspection after the Windows application's device is gone."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Capability, Client, ControlError, ErrorCode


def wait_until(fn, process, output, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = fn()
        if result:
            return result
        if process.poll() is not None:
            raise AssertionError(f'Probe exited early: {output.read_text(errors="replace")}')
        time.sleep(.02)
    raise AssertionError(f'Probe did not reach expected state: {output.read_text(errors="replace")}')


def export(client, reader, eye):
    ticket = client.start_export(reader, layer=eye, region=(0, 0, 64, 48, 32, 24))['export_id']
    try:
        deadline = time.monotonic() + 10
        while True:
            state = client.export_info(ticket)
            if state['phase'] != 'pending':
                break
            if time.monotonic() >= deadline:
                raise AssertionError('Inspection export did not complete')
            time.sleep(.01)
        assert state['phase'] == 'complete', state
        assert state['total_bytes'] == 32 * 24 * 4, state
        data = bytearray()
        while len(data) < state['total_bytes']:
            chunk = client.read_export(ticket, len(data), state['total_bytes'] - len(data))
            assert chunk['offset'] == len(data) and chunk['bytes'], chunk
            data.extend(chunk['bytes'])
        return bytes(data)
    finally:
        client.release_export(ticket)


def main():
    if os.environ.get('SIMXR_TEST_VALIDATE') != '1':
        print('SKIP Windows retained inspection requires Vulkan validation and a capable GPU')
        return 77
    probe, manifest = (str(Path(value).resolve()) for value in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='simxr-retained-windows-') as temporary:
        root = Path(temporary)
        endpoint = rf'\\.\pipe\SimXR.Control.retained.{os.getpid()}'
        output = root / 'probe.log'
        settings = root / 'vk_layer_settings.txt'
        settings.write_text('khronos_validation.validate_sync = true\n'
                            'khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG\n'
                            'khronos_validation.report_flags = error\n'
                            f'khronos_validation.log_filename = {(root / "validation.log").as_posix()}\n', encoding='utf-8')
        environment = dict(os.environ, XR_RUNTIME_JSON=manifest, SIMXR_DATA_DIR=temporary,
                           SIMXR_CONTROL_SOCKET=endpoint, SIMXR_VULKAN_VALIDATION='1',
                           VK_LAYER_SETTINGS_PATH=str(root), XR_PROBE_FRAMES='1200', XR_PROBE_SLEEP_MS='5',
                           DISABLE_XR_APILAYER_NOVENDOR_OBSMirror='1',
                           DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY='1')
        with output.open('wb') as log:
            process = subprocess.Popen([probe, '--retain-inspection'], env=environment,
                                       stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            try:
                def connect():
                    try:
                        return Client(endpoint, timeout=1)
                    except (OSError, TimeoutError):
                        return None

                with wait_until(connect, process, output) as client:
                    needed = Capability.IMAGE_HISTORY | Capability.IMAGE_EXPORT
                    def retain():
                        for candidate in client.sessions():
                            try:
                                if client.status(candidate)['capabilities'] & needed != needed:
                                    continue
                                history = client.image_history(candidate, count=10)['document']
                                for entry in reversed(history['entries']):
                                    if entry['phase'] != 'ready':
                                        continue
                                    try:
                                        value = client.retain_historical_image(candidate, entry['frame_id'])
                                        return value['image_reader_id'], entry['frame_id']
                                    except ControlError as error:
                                        if error.code not in (ErrorCode.BUSY, ErrorCode.NOT_FOUND):
                                            raise
                            except ControlError as error:
                                if error.code not in (ErrorCode.STALE_GENERATION, ErrorCode.NOT_FOUND):
                                    raise
                        return None

                    reader, frame = wait_until(retain, process, output)
                    original = [export(client, reader, eye) for eye in range(2)]
                    wait_until(lambda: 'READY application graphics destroyed' in output.read_text(errors='replace'),
                               process, output, timeout=45)
                    retired = 'SimXR inspection validation: all owned devices retired'
                    assert retired not in output.read_text(errors='replace'), 'Inspection device retired before the reader'
                    info = client.image_info(reader)
                    assert info['frame']['frame_id'] == frame, info
                    assert [export(client, reader, eye) for eye in range(2)] == original, 'Retained pixels changed'
                    assert client.release_image(reader)['released'], 'Reader release failed'
                wait_until(lambda: retired in output.read_text(errors='replace'), process, output, timeout=10)
                process.stdin.write(b'\n')
                process.stdin.flush()
                assert process.wait(timeout=20) == 0, output.read_text(errors='replace')
                report = output.read_text(errors='replace')
                assert retired in report, 'Inspection device retirement was not observed'
                assert 'validation layer verified live (deliberate error caught)' in report, report
                assert 'SimXR inspection Vulkan validation error' not in report, report
                assert '[FAIL]' not in report, report
                validation = root / 'validation.log'
                assert validation.is_file(), 'Validation layer settings were not loaded'
                assert not validation.read_text(errors='replace').strip(), validation.read_text(errors='replace')
                print('PASS Windows retained eye exports after application Vulkan teardown with inspection validation')
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if sys.exc_info()[0] is not None:
                    print(output.read_text(errors='replace'), file=sys.stderr)


if __name__ == '__main__':
    sys.exit(main())
