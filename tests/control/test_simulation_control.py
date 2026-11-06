"""Focused real endpoint/SDK/compiled-Luau command integration."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
from simxr_control import (Client, CommandKind, Capability, ControlError, ProtocolError, TransactionError,
                           collect_recording, to_luau, encode_command, decode_command, endpoint, Scenario)

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
    def test_endpoint_names_a_process_socket(self):
        self.assertEqual(Path(endpoint(42, '/tmp/simxr')), Path('/tmp/simxr/control.42.sock'))
        with self.assertRaises(ValueError):
            endpoint(0)
        with self.assertRaises(ValueError):
            Client('/tmp/simxr/control.42.sock', pid=42)

    def test_command_codec_preserves_editable_all_command_payloads(self):
        commands = [
            {'kind': 'take_control', 'channel': 'head', 'replace': True},
            {'kind': 'release_control', 'channel': 'head'},
            {'kind': 'head_pose', 'position': [1, 2, 3]},
            {'kind': 'head_pose', 'angular_velocity': [0, .5, 0]},
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

    def test_sdk_sources_apply_record_release_and_trace(self):
        with tempfile.TemporaryDirectory(prefix='simxr-commands-') as directory:
            path, marker = Path(directory) / 'control.sock', Path(directory) / 'start'
            finish = Path(directory) / 'finish'
            process = subprocess.Popen([HOST, str(path), str(marker), str(finish)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
                    with Client(path) as source:
                        source.start_recording(session)
                        queued = source.schedule(session,
                            [{'kind': 'take_control', 'channel': 'gaze'}, {'kind': 'gaze', 'yaw': .5}],
                            transaction_id=1, frame=2)
                        self.assertEqual(queued['phase'], 'queued')
                        step = client.submit(lease, CommandKind.STEP)
                        self.assertEqual(client.wait(session, step['operation_id'])['phase'], 'complete')
                        observed = source.simulation_status(session, 1)
                        self.assertEqual(observed['document']['applied_frame'], 2)
                        self.assertEqual(observed['phase'], 'applied')
                        blocked = client.schedule(session, [{'kind': 'take_control', 'channel': 'gaze'},
                                                            {'kind': 'gaze', 'yaw': .1}],
                                                  frame=client.next_frame(session, 1))
                        step = client.submit(lease, CommandKind.STEP)
                        self.assertEqual(client.wait(session, step['operation_id'])['phase'], 'complete')
                        with self.assertRaises(TransactionError) as held:
                            client.wait_applied(session, blocked['document']['transaction_id'])
                        self.assertEqual(held.exception.phase, 'conflict')
                        self.assertIn('gaze', held.exception.owners)
                        with self.assertRaises(ProtocolError):
                            source.schedule(session, [{'kind': 'gaze', 'ywa': .8}], transaction_id=2, frame=3)
                        source.release_control(session)
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
                finish.touch()
                output = process.communicate(timeout=5)[0]
                self.assertEqual(process.returncode, 0, output)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.communicate(timeout=5)


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

    def test_runner_rejects_invalid_arguments_and_source_before_connect(self):
        with tempfile.TemporaryDirectory(prefix='simxr-runner-input-') as directory:
            root = Path(directory)
            script = root / 'bounded.luau'
            script.write_text('simxr.checkpoint("valid source", true)')
            base = [RUNNER, '--socket', str(root / 'absent.sock'), '--rig', str(script)]
            for flags, detail in [(['--json'], 'Missing nonempty value'),
                                  (['--json', '--junit', 'report.xml'], 'Missing nonempty value'),
                                  (['--junit', ''], 'Missing nonempty value'),
                                  (['--session', '1'], 'must be supplied together'),
                                  (['--generation', '1'], 'must be supplied together'),
                                  (['--session', '0', '--generation', '1'], 'must be nonzero')]:
                with self.subTest(flags=flags):
                    result = subprocess.run(base + flags, capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(detail, result.stderr)
            nested = root
            while len(str(nested).encode('utf-8')) < 1024:
                nested /= 'nested-' + 'x' * 100
                nested.mkdir()
            long_script = nested / 'rig.luau'
            long_script.write_text('simxr.checkpoint("valid source", true)')
            result = subprocess.run(base[:-1] + [str(long_script)], capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn('Rig name exceeds 1024 bytes', result.stderr)
            script.write_text('')
            result = subprocess.run(base, capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn('Invalid bounded rig options', result.stderr)
            with script.open('wb') as source:
                source.truncate(64 * 1024 * 1024 + 1)
            result = subprocess.run(base, capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn('Rig source exceeds 64 MiB', result.stderr)
            result = subprocess.run(base[:-1] + [str(root)], capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertRegex(result.stderr, r'Cannot (open|read) rig:')

    def test_actual_headless_runner_socket_and_reports(self):
        with tempfile.TemporaryDirectory(prefix='simxr-luau-') as directory:
            path, marker = Path(directory) / 'control.sock', Path(directory) / 'start'
            script = Path(directory) / '入力-é.luau'
            report, junit = Path(directory) / '結果-é.json', Path(directory) / '結果-é.xml'
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
                report_probe = Path(directory) / 'report-write.luau'
                report_probe.write_text('simxr.checkpoint("report write", true)')
                failed = subprocess.run([RUNNER, '--socket', str(path), '--rig', str(report_probe),
                                         '--json', directory], capture_output=True, text=True, timeout=5)
                self.assertNotEqual(failed.returncode, 0, failed.stdout)
                self.assertEqual(json.loads(failed.stdout)['phase'], 'complete')
                self.assertIn('Cannot open report:', failed.stderr)
                self.assertTrue(Path(directory).is_dir())
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
