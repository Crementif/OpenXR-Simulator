"""Focused real endpoint/SDK/MCP/compiled-Luau command integration."""
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
sys.path.insert(0, str(ROOT / 'mcp-server'))
from simxr_control import (Client, CommandKind, Capability, ControlError, ProtocolError,
                           collect_recording, to_luau, encode_command, decode_command, Scenario)
from simxr_control.protocol import Tag

HOST, RUNNER = sys.argv[1:3]


def wait_for(predicate, process=None):
    deadline = time.monotonic() + 5
    while not predicate():
        if process is not None and process.poll() is not None:
            raise AssertionError(process.communicate()[0])
        if time.monotonic() >= deadline:
            raise AssertionError('Timed out waiting for real endpoint state')
        time.sleep(0.005)


class SimulationTests(unittest.TestCase):
    def test_command_codec_preserves_editable_all_command_payloads(self):
        commands = [
            {'kind': 'take_control', 'channel': 'head', 'replace': True},
            {'kind': 'release_control', 'channel': 'head'},
            {'kind': 'head_pose', 'position': [1, 2, 3]},
            {'kind': 'controller_pose', 'hand': 1, 'connected': False},
            {'kind': 'controller_controls', 'hand': 0, 'trigger': .5, 'primary': True},
            {'kind': 'body', 'preset': 1, 'joints': [{'joint': 3, 'tracked': False, 'position': [1, 2, 3]}]},
            {'kind': 'gaze', 'yaw': .25, 'tracked': False},
            {'kind': 'tracker', 'identity': 2**64 - 1, 'role': 6, 'anchor': 12, 'position': [1, 2, 3]},
            {'kind': 'tracker', 'identity': 1, 'anchor': 0,
             'joint_offset': {'position': [.125, .25, -.5], 'orientation': [0, 0, 0, 1]}},
            {'kind': 'views', 'ipd': .065},
            {'kind': 'proportions', 'height': 1.9},
            {'kind': 'controller_profile', 'hand': 1, 'path': '/interaction_profiles/oculus/touch_controller'},
            {'kind': 'scenario', 'scenario': 'gaze_unavailable', 'enabled': True},
            {'kind': 'reference_spaces', 'local_position': [2, 1, 3], 'bounds_available': False},
        ]
        for command in commands:
            field = encode_command(command)
            decoded = decode_command({'encoding': 'simxr-command-v1-hex', 'data': field.value.hex()})
            self.assertEqual(encode_command(decoded).value, field.value, command)
        with self.assertRaises(ProtocolError):
            encode_command({'kind': 'gaze', 'ywa': .4})

    def test_sdk_and_mcp_apply_record_release_and_trace(self):
        spec = importlib.util.spec_from_file_location('simulation_debug_adapter', ROOT / 'mcp-server/debug_control.py')
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        with tempfile.TemporaryDirectory(prefix='simxr-commands-') as directory:
            path, marker = Path(directory) / 'control.sock', Path(directory) / 'start'
            process = subprocess.Popen([HOST, str(path), str(marker)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                wait_for(path.exists, process)
                with Client(path) as client:
                    session = client.sessions()[0]
                    self.assertTrue(client.hello['capabilities'] & Capability.SIMULATION_COMMANDS)
                    lease = client.acquire(session)
                    pause = client.submit(lease, CommandKind.PAUSE)
                    self.assertEqual(client.wait(session, pause['operation_id'])['phase'], 'complete')
                    client.start_recording(session)
                    with self.assertRaises(ControlError) as repeated:
                        client.start_recording(session)
                    self.assertEqual(repeated.exception.code.name, 'CONFLICT')
                    with Client(path) as observer:
                        with self.assertRaises(ControlError) as blocked_start:
                            observer.start_recording(session)
                        self.assertEqual(blocked_start.exception.code.name, 'BUSY')
                        with self.assertRaises(ControlError) as blocked_stop:
                            observer.stop_recording(session)
                        self.assertEqual(blocked_stop.exception.code.name, 'BUSY')
                    commands = [{'kind': 'take_control', 'channel': 'head'},
                                {'kind': 'head_pose', 'position': [2, 1.7, -3]},
                                {'kind': 'take_control', 'channel': 'left_controls'},
                                {'kind': 'controller_controls', 'hand': 0, 'trigger': .75, 'primary': True},
                                {'kind': 'take_control', 'channel': 'trackers'},
                                {'kind': 'tracker', 'index': 0, 'identity': 1, 'role': 0,
                                 'position': [1, 1, -1]}]
                    queued = client.schedule(session, commands, frame=1, transaction_id=1)
                    self.assertEqual(queued['phase'], 'queued')
                    marker.touch()
                    step = client.submit(lease, CommandKind.STEP)
                    self.assertEqual(client.wait(session, step['operation_id'])['phase'], 'complete')
                    applied = client.wait_simulation(session, 1)
                    self.assertEqual(applied['phase'], 'applied')
                    state = client.simulation_status(session)['document']
                    self.assertEqual(state['configuration']['head']['position'][0], 2)
                    self.assertEqual(state['configuration']['controllers'][0]['controls']['trigger'], .75)
                    trackers = state['configuration']['trackers']
                    self.assertEqual(trackers[0]['pose_override']['position'], [1, 1, -1])
                    self.assertIsNone(trackers[0]['anchor'])
                    self.assertEqual(trackers[1]['anchor'], 0)
                    self.assertIsNone(trackers[1]['pose_override'])
                    changed = client.schedule(session, [{'kind': 'head_pose', 'position': [9, 9, 9]}], frame=1, transaction_id=1)
                    self.assertEqual(changed['phase'], 'conflict')
                    client.stop_recording(session)
                    recording = collect_recording(client, session)
                    self.assertEqual(recording['total'], 1)
                    normalized = [decode_command(command) for command in recording['transactions'][0]['commands']]
                    self.assertEqual(normalized[1]['position'][0], 2)
                    exported = to_luau(recording)
                    self.assertIn('simxr.schedule', exported)
                    self.assertIn('head_pose', exported)
                    self.assertIn('controller_controls', exported)
                    self.assertNotIn('simxr-command-v1-hex', exported)
                    diagnostics = client.diagnostics(session)['document']
                    self.assertTrue(any(event['kind'] == 'commands_applied' for event in diagnostics['events']))
                    self.assertTrue(any(event['kind'] == 'submitted' for event in diagnostics['events']))
                    tools = adapter.DebugTools()
                    try:
                        connected = tools.call('debug_connect', {'path': str(path)})
                        self.assertTrue(connected['ok'], connected)
                        common = {'connection': connected['connection'], **connected['sessions'][0]}
                        self.assertTrue(tools.call('simulation_start_recording', common)['ok'])
                        queued = tools.call('simulation_schedule', {**common, 'transaction_id': '1', 'frame': '2',
                            'commands': [{'kind': 'take_control', 'channel': 'gaze'}, {'kind': 'gaze', 'yaw': .5}]})
                        self.assertTrue(queued['ok'], queued)
                        step = client.submit(lease, CommandKind.STEP)
                        self.assertEqual(client.wait(session, step['operation_id'])['phase'], 'complete')
                        observed = tools.call('simulation_status', {**common, 'transaction_id': '1'})
                        self.assertTrue(observed['ok'], observed)
                        self.assertEqual(observed['document']['applied_frame'], '2')
                        self.assertEqual(observed['phase'], 'applied')
                        invalid = tools.call('simulation_schedule', {**common, 'transaction_id': '2', 'frame': '3',
                            'commands': [{'kind': 'gaze', 'ywa': .8}]})
                        self.assertFalse(invalid['ok'])
                        self.assertTrue(tools.call('simulation_release_control', common)['ok'])
                    finally:
                        tools.close()
                    wait_for(lambda: not client.read_recording(session, count=0)['document']['active'])
                    client.start_recording(session)
                    client.stop_recording(session)
                    client.release_control(session)
                    step = client.submit(lease, CommandKind.STEP)
                    self.assertEqual(client.wait(session, step['operation_id'])['phase'], 'complete')
                    state = client.simulation_status(session)['document']
                    self.assertEqual(state['configuration']['controllers'][0]['controls']['trigger'], 0)
                    self.assertTrue(all(owner is None for owner in state['owners']))
                    resume = client.submit(lease, CommandKind.RESUME)
                    self.assertEqual(client.wait(session, resume['operation_id'])['phase'], 'complete')
                output = process.communicate(timeout=5)[0]
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.communicate(timeout=5)

    def test_compatibility_tools_use_shared_acknowledged_commands(self):
        spec = importlib.util.spec_from_file_location('compatibility_control', ROOT / 'mcp-server/legacy_control.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory(prefix='simxr-compatibility-') as directory:
            path, marker = Path(directory) / 'control.123.sock', Path(directory) / 'start'
            tools = module.LegacyControl(directory)
            self.assertIsNone(tools.call('set_head_pose', {}))
            host = subprocess.Popen([HOST, str(path), str(marker)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                wait_for(path.exists, host)
                with Client(path) as client:
                    session = client.sessions()[0]
                    lease = client.acquire(session)
                    pause = client.submit(lease, CommandKind.PAUSE)
                    self.assertEqual(client.wait(session, pause['operation_id'])['phase'], 'complete')
                    queued = tools.call('set_head_pose', {'x': 2, 'y': 1.8, 'z': -3, 'yaw_deg': 90})
                    self.assertTrue(queued['ok'], queued)
                    self.assertEqual(queued['phase'], 'queued')
                    self.assertFalse(queued['applied'])
                    self.assertFalse((Path(directory) / 'head_pose_command.json').exists())
                    marker.touch()
                    def step():
                        operation = client.submit(lease, CommandKind.STEP)
                        self.assertEqual(client.wait(session, operation['operation_id'])['phase'], 'complete')
                        return client.simulation_status(session)['document']['configuration']
                    state = step()
                    self.assertEqual(state['head']['position'][0], 2)
                    self.assertAlmostEqual(state['head']['orientation'][1], 2**-.5, places=5)
                    queued = tools.call('set_controller_pose', {'hand': 0, 'posX': 0, 'posY': 0, 'posZ': -1, 'trigger': .5})
                    self.assertTrue(queued['ok'], queued)
                    pending = tools.call('hold_controller_inputs', {'hand': 0, 'grip': .75})
                    self.assertFalse(pending['ok'], pending)
                    self.assertEqual(pending['transaction_id'], queued['transaction_id'])
                    state = step()
                    self.assertAlmostEqual(state['controllers'][0]['pose']['position'][0], 1, places=5)
                    self.assertEqual(state['controllers'][0]['controls']['trigger'], .5)
                    queued = tools.call('hold_controller_inputs', {'hand': 0, 'grip': .75, 'primary': 1})
                    self.assertTrue(queued['ok'], queued)
                    state = step()
                    self.assertEqual(state['controllers'][0]['controls']['trigger'], .5)
                    self.assertEqual(state['controllers'][0]['controls']['grip'], .75)
                    self.assertTrue(state['controllers'][0]['controls']['primary'])
                    original_fov = state['views']['outer_fov']
                    queued = tools.call('set_ipd', {'ipd_mm': 70})
                    self.assertTrue(queued['ok'], queued)
                    state = step()
                    self.assertAlmostEqual(state['views']['ipd'], .07)
                    self.assertEqual(state['views']['outer_fov'], original_fov)
                    eye = {'left_deg': -40, 'right_deg': 50, 'up_deg': 45, 'down_deg': -45}
                    queued = tools.call('set_fov', {'left_eye': eye, 'right_eye': eye})
                    self.assertTrue(queued['ok'], queued)
                    state = step()
                    self.assertAlmostEqual(state['views']['ipd'], .07)
                    self.assertAlmostEqual(state['views']['outer_fov'][0], math.radians(-40), places=5)
                    tools.close()
                    # The applied frame remains historical after connection cleanup.
                    self.assertEqual(client.simulation_status(session)['document']['frame_id'], 5)
                    resumed = client.submit(lease, CommandKind.RESUME)
                    self.assertGreater(resumed['operation_id'], 0)
                output = host.communicate(timeout=5)[0]
                self.assertEqual(host.returncode, 0, output)
            finally:
                tools.close()
                if host.poll() is None:
                    host.terminate()
                host.communicate(timeout=5)

    def test_python_saved_recording_replays_with_application_and_display_steps(self):
        with tempfile.TemporaryDirectory(prefix='simxr-python-recording-') as directory:
            root = Path(directory)
            source_path, source_marker = root / 'source.sock', root / 'source-start'
            source_finish = root / 'source-finish'
            source = subprocess.Popen([HOST, str(source_path), str(source_marker), str(source_finish)],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            replay = runner = None
            try:
                wait_for(source_path.exists, source)
                with Client(source_path) as client:
                    session = client.sessions()[0]
                    lease = client.acquire(session)
                    pause = client.submit(lease, CommandKind.PAUSE)
                    self.assertEqual(client.wait(session, pause['operation_id'])['phase'], 'complete')
                    client.schedule(session, [{'kind': 'take_control', 'channel': 'head'},
                                              {'kind': 'scenario', 'scenario': 'head_untracked'},
                                              {'kind': 'take_control', 'channel': 'session'},
                                              {'kind': 'reference_spaces', 'local_position': [2, 1, 3],
                                               'bounds_available': False}],
                                    frame=1, transaction_id=1)
                    source_marker.touch()
                    first = client.submit(lease, CommandKind.STEP)
                    self.assertEqual(client.wait(session, first['operation_id'])['phase'], 'complete')
                    client.start_recording(session)
                    for frame in range(2, 5):
                        commands = [{'kind': 'head_pose', 'position': [frame - 1, 1.75, -frame]}]
                        if frame == 3:
                            commands.append({'kind': 'scenario', 'scenario': 'head_untracked', 'enabled': False})
                        client.schedule(session, commands, frame=frame, transaction_id=frame)
                        step = client.submit(lease, CommandKind.STEP)
                        self.assertEqual(client.wait(session, step['operation_id'])['phase'], 'complete')
                        self.assertEqual(client.wait_simulation(session, frame)['phase'], 'applied')
                    client.stop_recording(session)
                    document = collect_recording(client, session)
                    self.assertEqual(document['total'], 3)
                    self.assertEqual(document['origin_frame'], 1)
                    self.assertEqual(document['initial_scenarios'], [int(Scenario.HEAD_UNTRACKED)])
                    initial_head = next(decode_command(command) for command in document['initial_commands']
                                        if decode_command(command)['kind'] == 'head_pose')
                    self.assertEqual(initial_head['flags'], 15)
                    self.assertEqual(document['initial']['reference_spaces']['local']['position'], [2, 1, 3])
                    self.assertIsNone(document['initial']['reference_spaces']['stage_bounds'])
                    script = to_luau(document)
                    self.assertIn('simxr.step_display()', script)
                    resumed = client.submit(lease, CommandKind.RESUME)
                    self.assertEqual(client.wait(session, resumed['operation_id'])['phase'], 'complete')
                source_finish.touch()
                source_output = source.communicate(timeout=5)[0]
                self.assertEqual(source.returncode, 0, source_output)

                replay_path, replay_marker = root / 'replay.sock', root / 'replay-start'
                replay_finish = root / 'replay-finish'
                rig, report = root / 'recorded.luau', root / 'report.json'
                rig.write_text(script, encoding='utf-8')
                replay = subprocess.Popen([HOST, str(replay_path), str(replay_marker), str(replay_finish)],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                wait_for(replay_path.exists, replay)
                runner = subprocess.Popen([RUNNER, '--socket', str(replay_path), '--rig', str(rig),
                                           '--seed', str(document['seed']), '--json', str(report)],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                with Client(replay_path) as observer:
                    session = observer.sessions()[0]
                    wait_for(lambda: observer.status(session)['manual'], runner)
                    replay_marker.touch()
                output = runner.communicate(timeout=8)[0]
                self.assertEqual(runner.returncode, 0, output)
                result = json.loads(report.read_text())
                self.assertEqual(result['phase'], 'complete')
                self.assertEqual(result['transactions'], 4)
                self.assertEqual(len(result['checkpoints']), 1)
                self.assertTrue(all(item['passed'] for item in result['checkpoints']), result)
                replay_finish.touch()
                replay_output = replay.communicate(timeout=5)[0]
                self.assertEqual(replay.returncode, 0, replay_output)
            finally:
                for process in (runner, replay, source):
                    if process is not None and process.poll() is None:
                        process.terminate()
                    if process is not None:
                        process.communicate(timeout=5)

    def test_actual_headless_runner_socket_and_reports(self):
        with tempfile.TemporaryDirectory(prefix='simxr-luau-') as directory:
            path, marker = Path(directory) / 'control.sock', Path(directory) / 'start'
            script = Path(directory) / 'input.luau'
            report, junit = Path(directory) / 'run.json', Path(directory) / 'run.xml'
            script.write_text('''simxr.pause()
local id = simxr.schedule({frame="1", commands={
    {kind="take_control",channel="gaze"},
    {kind="gaze",yaw=0.25,pitch=-0.1},
}})
simxr.step()
local applied = simxr.await_transaction(id)
simxr.checkpoint("actual socket frame", applied.applied_frame == "1")
simxr.checkpoint("actual gaze", simxr.state().configuration.gaze.yaw == 0.25)
''')
            host = subprocess.Popen([HOST, str(path), str(marker)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            runner = None
            try:
                wait_for(path.exists, host)
                runner = subprocess.Popen([RUNNER, '--socket', str(path), '--rig', str(script), '--seed', '83',
                                           '--json', str(report), '--junit', str(junit)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                with Client(path) as observer:
                    session = observer.sessions()[0]
                    wait_for(lambda: observer.status(session)['manual'], runner)
                    marker.touch()
                output = runner.communicate(timeout=8)[0]
                self.assertEqual(runner.returncode, 0, output)
                result = json.loads(report.read_text())
                self.assertEqual(result['phase'], 'complete')
                self.assertEqual(result['seed'], '83')
                self.assertEqual(len(result['checkpoints']), 2)
                self.assertIn('failures="0"', junit.read_text())
                output = host.communicate(timeout=5)[0]
                self.assertEqual(host.returncode, 0, output)
            finally:
                for process in (runner, host):
                    if process is not None and process.poll() is None:
                        process.terminate()
                        process.communicate(timeout=5)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
