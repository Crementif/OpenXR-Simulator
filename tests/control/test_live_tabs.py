"""Run the portable multi-tab workflow against independently running native probes."""
import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk' / 'python'))
from simxr_control import Capability, Client


def wait_for_frame(endpoint, process):
    deadline = time.monotonic() + 25
    failure = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'Producer exited before its first frame: {process.returncode}')
        try:
            with Client(endpoint, timeout=1, required=0, name='Live tab readiness') as client:
                for session in client.sessions():
                    needed = Capability.GPU_PREVIEW | Capability.LIVE_PREVIEW
                    status = client.status(session)
                    if status['capabilities'] & needed != needed:
                        raise RuntimeError('Producer does not support native GPU preview')
                    history = client.frame_history(session, after=0, count=128)
                    if any(frame.get('stage') == 'submitted' for frame in history.get('frames', [])):
                        return
        except Exception as error:
            failure = error
        time.sleep(.1)
    raise RuntimeError(f'No submitted frame from {endpoint}: {failure!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True, help='Native OpenXR probe executable')
    parser.add_argument('--interaction', type=Path, required=True, help='Companion interaction test executable')
    parser.add_argument('--runtime', type=Path, required=True, help='Simulator runtime manifest')
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--count', type=int, choices=(2, 4), default=2)
    parser.add_argument('--frames', type=int, default=1800)
    scenarios = parser.add_mutually_exclusive_group()
    scenarios.add_argument('--snapshot-pressure', action='store_true', help='Exercise the shared budget with four apps')
    scenarios.add_argument('--replay-close', action='store_true', help='Close a background recording replay')
    args = parser.parse_args()
    if args.frames < 100:
        parser.error('Use at least 100 frames for the live workflow')
    if args.snapshot_pressure and args.count != 4:
        parser.error('Snapshot pressure requires --count 4')
    probe, interaction, runtime = (path.resolve(strict=True) for path in (args.probe, args.interaction, args.runtime))
    output = args.output.absolute()
    output.mkdir(parents=False, exist_ok=False)
    environment = dict(os.environ)
    for name in list(environment):
        if name.startswith(('SIMXR_PROBE_', 'XR_PROBE_', 'SIMXR_TABS_')) or (
                name.startswith('SIMXR_') and name.endswith('_QA')):
            environment.pop(name)
    environment.update(XR_RUNTIME_JSON=str(runtime), SIMXR_VULKAN_VALIDATION='1', SIMXR_UI_VALIDATION='1',
                       SIMXR_DATA_DIR=str(output), SIMXR_COMPANION_SETTINGS=str(output / 'settings.json'),
                       SIMXR_TABS_LIVE_DIRECTORY=str(output),
                       DISABLE_XR_APILAYER_NOVENDOR_OBSMirror='1',
                       DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY='1',
                       VK_LOADER_LAYERS_DISABLE='VK_LAYER_OBS_HOOK,VK_LAYER_EOS_Overlay,VK_LAYER_VALVE_steam_overlay,VK_LAYER_VALVE_steam_fossilize')
    if args.snapshot_pressure:
        environment['SIMXR_TABS_BUDGET_QA'] = '1'
    if args.replay_close:
        environment['SIMXR_TABS_PORTABLE_REPLAY_QA'] = '1'
    for name in ('SIMXR_DETERMINISTIC', 'SIMXR_CONTROL_SOCKET', 'XR_PROBE_REQUIRE_INPUT', 'XR_PROBE_RESIZE'):
        environment.pop(name, None)
    environment['PATH'] = os.pathsep.join((str(probe.parent), str(interaction.parent), str(runtime.parent),
                                          environment.get('PATH', '')))
    if os.name == 'nt':
        environment.update(XR_PROBE_FRAMES=str(args.frames), XR_PROBE_SESSIONS='1')
    else:
        environment.update(SIMXR_PROBE_FRAMES=str(args.frames), SIMXR_PROBE_INTERACTIVE='1',
                           SIMXR_PROBE_GPU_INSPECTION='1')
    processes = []
    try:
        with ExitStack() as stack:
            for index in range(args.count):
                data = output / f'app-{index + 1}'
                data.mkdir()
                log = stack.enter_context((output / f'app-{index + 1}.log').open('w', encoding='utf-8'))
                child_env = dict(environment, SIMXR_DATA_DIR=str(data))
                process = subprocess.Popen([str(probe)], env=child_env, stdout=log, stderr=subprocess.STDOUT)
                processes.append(process)
                endpoint = (rf'\\.\pipe\SimXR.Control.{process.pid}' if os.name == 'nt'
                            else str(data / f'control.{process.pid}.sock'))
                environment['SIMXR_TABS_LIVE_' + ('FIRST', 'SECOND', 'THIRD', 'FOURTH')[index]] = endpoint
            for process, name in zip(processes, ('FIRST', 'SECOND', 'THIRD', 'FOURTH')):
                wait_for_frame(environment['SIMXR_TABS_LIVE_' + name], process)
            with (output / 'interaction.log').open('w', encoding='utf-8') as log:
                result = subprocess.run([str(interaction)], env=environment, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=90, check=False)
            report = (output / 'interaction.log').read_text(encoding='utf-8', errors='replace')
            marker = ('PASS shared tab snapshot budget' if args.snapshot_pressure else
                      'PASS portable replay close' if args.replay_close else 'PASS portable live tabs')
            if result.returncode or marker not in report:
                raise RuntimeError(f'Live tab interaction failed ({result.returncode}):\n{report}')
            if 'VUID-' in report or 'preserving' in report or report.count('normalReadback=0') < args.count:
                raise RuntimeError(f'Live tab GPU validation or preview failed:\n{report}')
            for process in processes:
                if process.wait(timeout=120):
                    raise RuntimeError(f'Producer {process.pid} exited with {process.returncode}')
        for index in range(args.count):
            report = (output / f'app-{index + 1}.log').read_text(encoding='utf-8', errors='replace')
            diagnostics = report.replace('no Vulkan validation errors across the frame loop', '')
            if ('VUID-' in diagnostics or 'Vulkan validation error' in diagnostics or 'preserving' in diagnostics or
                    'VK_LAYER_KHRONOS_validation not installed' in report):
                raise RuntimeError(f'Producer {index + 1} reported a GPU validation or retirement failure:\n{report}')
            completed = ('0 failure(s)' in report and 'VK_LAYER_KHRONOS_validation available' in report
                         if probe.stem == 'xr_probe_vk' else 'PASS:' in report)
            if not completed:
                raise RuntimeError(f'Producer {index + 1} did not confirm normal teardown:\n{report}')
        label = 'snapshot budget with' if args.snapshot_pressure else 'background replay close with' if args.replay_close else 'live tabs with'
        print(f'PASS {label} {args.count} native applications; evidence: {output}')
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == '__main__':
    main()
