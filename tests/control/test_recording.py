"""CPU-only recording paging and editable exact-frame Luau export tests."""
from copy import deepcopy
from pathlib import Path
import sys
import subprocess
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Session, ProtocolError, collect_recording, to_luau, encode_command, decode_command
from simxr_control import recording as implementation
from simxr_control.protocol import Field, Tag, decode_fields, encode_fields, to_dict
from simxr_control.commands import Scenario
from simxr_control.body_observation import JOINT_SETS

from simxr_control import Client
from simxr_control.protocol import Capability, OpCode


class RecordingDrainClientTests(unittest.TestCase):
    def test_explicit_identity_and_absolute_offsets(self):
        client = Client.__new__(Client)
        client.hello = {'capabilities': Capability.RECORDING_DRAIN}
        client.request = lambda opcode, fields: (opcode, {field.tag: field for field in fields})
        opcode, fields = client.start_recording(SESSION, drain=True)
        self.assertEqual(opcode, OpCode.START_RECORDING)
        self.assertTrue(fields[Tag.RECORDING_DRAIN].as_bool())
        opcode, fields = client.read_recording(SESSION, recording_id=7, offset=54000)
        self.assertEqual(fields[Tag.RECORDING_ID].as_u64(), 7)
        self.assertEqual(fields[Tag.OFFSET].as_u64(), 54000)
        _, fields = client.read_recording(SESSION, recording_id=7, offset=None)
        self.assertNotIn(Tag.OFFSET, fields)
        opcode, fields = client.acknowledge_recording(SESSION, 7, 54016)
        self.assertEqual(opcode, OpCode.ACKNOWLEDGE_RECORDING)
        self.assertEqual(fields[Tag.OFFSET].as_u64(), 54016)
        _, fields = client.stop_recording(SESSION, recording_id=7)
        self.assertEqual(fields[Tag.RECORDING_ID].as_u64(), 7)
        for invalid in (0, -1, True, 2**64):
            with self.assertRaises(ProtocolError):
                client.acknowledge_recording(SESSION, invalid, 0)
        with self.assertRaises(ProtocolError):
            client.read_recording(SESSION, offset=54000)
        client.hello = {'capabilities': 0}
        with self.assertRaises(ProtocolError):
            client.start_recording(SESSION, drain=True)
        with self.assertRaises(ProtocolError):
            client.acknowledge_recording(SESSION, 7, 0)


U64_MAX = 2**64 - 1
SESSION = Session(71, 23)


def encoded(command):
    return {'encoding': 'simxr-command-v1-hex', 'data': encode_command(command).value.hex()}


def outcome(identity=1, frame=11, commands=None, phase='applied'):
    return {'transaction_id': identity, 'phase': phase, 'applied_frame': frame,
            'applied_time': 7000, 'source': {'kind': 1, 'id': U64_MAX},
            'target_frame': frame, 'target_time': None, 'effective_frame': frame,
            'effective_time': None, 'late_policy': 0, 'recording_overrun': False,
            'commands': [encoded(command) for command in (commands or [{'kind': 'gaze', 'yaw': .25}])]}


def recording(transactions=None, **changes):
    entries = [outcome()] if transactions is None else transactions
    value = {'version': 1, 'active': False, 'overflow': False, 'seed': U64_MAX,
             'origin_frame': 10, 'origin_time': -1234, 'total': len(entries),
             'initial': {'head': {'position': [1.0, 1.7, -2.0]}, 'profile': 'original'},
             'initial_commands': [encoded({'kind': 'head_pose', 'position': [1, 1.7, -2]}),
                                  encoded({'kind': 'reference_spaces'})],
             'initial_scenarios': [], 'transactions': entries}
    value.update(changes)
    return value


def page(document, offset=0, end=True, **changes):
    value = {'session': SESSION.id, 'generation': SESSION.generation, 'offset': offset,
             'length': len(document['transactions']), 'end': end, 'document': document}
    value.update(changes)
    return value


class ReleaseSnapshotTests(unittest.TestCase):
    def document(self):
        entry = outcome(frame=11, commands=[dict(kind='release_control', channel=0)])
        entry.update(after_frame=False, applied_runtime_time=2**53 + 17,
                     release_state=dict(schema=1, frame=11, runtime_time=2**53 + 17,
                                        initial_commands=[encoded(dict(kind='eye_views', enabled=False))],
                                        scenarios=[]))
        return recording([entry], version=3)

    def test_optional_snapshot_and_exact_anchor(self):
        document = self.document()
        implementation._validate_document(document)
        collected = collect_recording(Pages([page(document)]), SESSION)
        self.assertEqual(collected['transactions'][0]['release_state'],
                         document['transactions'][0]['release_state'])
        del document['transactions'][0]['release_state']
        implementation._validate_document(document)

    def test_invalid_snapshot(self):
        for field, value in [('schema', True), ('schema', 2), ('frame', 12),
                             ('runtime_time', 2**53 + 18), ('runtime_time', None),
                             ('initial_commands', None), ('scenarios', [0, 0]),
                             ('scenarios', [len(Scenario)]), ('scenarios', [True])]:
            with self.subTest(field=field, value=value):
                document = self.document()
                document['transactions'][0]['release_state'][field] = value
                with self.assertRaises(ProtocolError):
                    implementation._validate_document(document)
        document = self.document()
        document['transactions'][0]['commands'] = [encoded(dict(kind='head_pose'))]
        with self.assertRaises(ProtocolError):
            implementation._validate_document(document)


class EyeCommandTests(unittest.TestCase):
    def test_trigger_click_roundtrip(self):
        for value in (False, True):
            command = dict(kind='controller_controls', hand=0, trigger=.25, trigger_click=value,
                           physical_profile='/interaction_profiles/valve/index_controller')
            decoded = decode_command(encoded(command))
            self.assertIs(decoded['trigger_click'], value)
            self.assertEqual(decoded['physical_profile'], command['physical_profile'])
            self.assertEqual(decoded['trigger'], .25)
        self.assertNotIn('trigger_click', decode_command(encoded(dict(kind='controller_controls'))))

    def test_eye_command_roundtrip(self):
        eye = dict(position=[-.03125, 1.5, 0], orientation=[0, 0, 0, 1], flags=15,
                   fov=[-.5, .75, .75, -.5])
        command = dict(kind='eye_views', enabled=True, space_revision=U64_MAX,
                       left=eye, right=dict(eye, flags=0))
        self.assertEqual(decode_command(encoded(command)), command)
        clear = dict(kind='eye_views', enabled=False)
        self.assertEqual(decode_command(encoded(clear)), clear)
        with self.assertRaises(ProtocolError):
            encode_command(dict(clear, left=eye))


class TrackerCollectionTests(unittest.TestCase):
    def test_collection_roundtrip_and_luau(self):
        devices = [dict(identity=100 + slot, role=4, position=[slot, 1, 0]) for slot in range(64)]
        command = dict(kind='trackers', devices=devices)
        decoded = decode_command(encoded(command))
        self.assertEqual(len(decoded['devices']), 64)
        self.assertEqual(decoded['devices'][-1]['identity'], 163)
        self.assertEqual(encoded(decoded), encoded(command))
        self.assertEqual(decode_command(encoded(dict(kind='trackers', devices=[])))['devices'], [])
        event = outcome(commands=[command])
        event["after_frame"] = False
        document = recording([event], version=3)
        script = to_luau(document)
        self.assertIn('["kind"] = "trackers"', script)
        self.assertIn('["identity"] = 163', script)
        for version in (1, 2):
            with self.assertRaises(ProtocolError):
                to_luau(dict(document, version=version))

    def test_tracker_source_path(self):
        path = 'htcx:/device/physical_163'
        command = dict(kind='tracker', identity=163, source_path=path)
        self.assertEqual(decode_command(encoded(command))['source_path'], path)
        collection = dict(kind='trackers', devices=[command])
        self.assertEqual(decode_command(encoded(collection))['devices'][0]['source_path'], path)
        self.assertNotIn('source_path', decode_command(encoded(dict(kind='tracker', identity=1))))
        for invalid in ('missing_namespace', 'htcx:/', 'htcx:/bad space', 'htcx:/bad\0path', 'htcx:/' + 'a' * 282):
            with self.assertRaises(ProtocolError):
                encode_command(dict(command, source_path=invalid))

    def test_tracker_partial_validity(self):
        for flags in (0, 1, 2, 5, 10):
            command = dict(kind='tracker', identity=123, flags=flags)
            self.assertEqual(decode_command(encoded(command))['flags'], flags)
            collection = dict(kind='trackers', devices=[command])
            self.assertEqual(decode_command(encoded(collection))['devices'][0]['flags'], flags)
        self.assertNotIn('flags', decode_command(encoded(dict(kind='tracker', identity=123))))
        with self.assertRaises(ProtocolError):
            encode_command(dict(kind='tracker', identity=123, flags=16))

    def test_applied_frame_collection(self):
        command = dict(kind='trackers', devices=[dict(identity=100 + i) for i in range(64)])
        applied = Field.nested(Tag.APPLIED_INPUT, [encode_command(command, Tag.TRACKER_COLLECTION)])
        result = to_dict([applied])['applied_input']['commands']
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]['devices'][-1]['identity'], 163)

    def test_collection_bounds_and_indexes(self):
        for devices in (None, {}, [dict(identity=i + 1) for i in range(65)],
                        [dict(identity=1, index=1)], [dict(kind='trackers', devices=[])]):
            with self.assertRaises(ProtocolError):
                encode_command(dict(kind='trackers', devices=devices))
        command = encode_command(dict(kind='trackers', devices=[dict(identity=1), dict(identity=2)]))
        outer = decode_fields(command.value)
        entries = list(decode_fields(outer[Tag.VALUES].value).values())
        entries[1] = Field.nested(3, list(decode_fields(entries[1].value).values()), True)
        outer[Tag.VALUES] = Field.nested(Tag.VALUES, entries, True)
        with self.assertRaises(ProtocolError):
            decode_command(dict(encoding='simxr-command-v1-hex', data=encode_fields(list(outer.values())).hex()))


class BodyObservationTests(unittest.TestCase):
    @staticmethod
    def sample(joint_set=0, located=True):
        body = dict(joint_set=joint_set, requested_time=2**53 + 1, located=located,
                    joints=[dict(joint=j, position=[j, 0, 0], orientation=[0, 0, 0, 1], flags=0)
                            for j in sorted(JOINT_SETS[joint_set])])
        if located:
            if joint_set < 2:
                body.update(returned_time=0, active=False, confidence=0, skeleton_revision=0,
                            skeleton=[dict(joint=j, position=[0, 0, 0], orientation=[0, 0, 0, 1])
                                      for j in sorted(JOINT_SETS[joint_set])])
            else:
                body['all_joint_poses_tracked'] = False
                body['joints'][0]['flags'] = 1
        return body

    def test_roundtrip_and_recording_schema(self):
        for joint_set in range(4):
            for located in (False, True):
                body = self.sample(joint_set, located)
                command = dict(kind='body', observation=body)
                decoded = decode_command(encoded(command))
                self.assertEqual(decoded['observation'], body)
                event = dict(outcome(commands=[command]), after_frame=False, applied_runtime_time=2**53 + 7)
                document = recording([event], version=4, origin_runtime_time=2**53 + 3)
                script = to_luau(document)
                self.assertIn('["requested_time"] = "9007199254740993"', script)
                self.assertIn('["observation_origin"] = "9007199254740999"', script)
                unanchored = deepcopy(document)
                del unanchored['transactions'][0]['applied_runtime_time']
                with self.assertRaises(ProtocolError):
                    to_luau(unanchored)
                for version in (1, 2, 3):
                    invalid = deepcopy(document)
                    invalid['version'] = version
                    with self.assertRaises(ProtocolError):
                        to_luau(invalid)
                document['initial_commands'].append(encoded(command))
                self.assertIn('["observation"]', to_luau(document))
                document['version'] = 3
                with self.assertRaises(ProtocolError):
                    to_luau(document)

    def test_replay_anchor(self):
        command = dict(kind='body', observation=self.sample(), observation_origin=2**53 + 1)
        decoded = decode_command(encoded(command))
        self.assertEqual(decoded['observation_origin'], 2**53 + 1)
        self.assertEqual(encode_command(decoded).value, encode_command(command).value)
        for origin in (0, -1, 2**63, float(2**53), True):
            with self.assertRaises(ProtocolError):
                encode_command(dict(command, observation_origin=origin))
        with self.assertRaises(ProtocolError):
            encode_command(dict(kind='body', observation_origin=1))

    def test_malformed_edits(self):
        mutations = [lambda b: b.update(joint_set=256),
                     lambda b: b.update(requested_time=2**63),
                     lambda b: b.update(requested_time=float(2**53)),
                     lambda b: b.update(active=None),
                     lambda b: b.update(confidence=float('nan')),
                     lambda b: b['joints'].pop(),
                     lambda b: b['joints'].append(b['joints'][0]),
                     lambda b: b['joints'][0].pop('flags'),
                     lambda b: b['joints'][0].update(flags=15),
                     lambda b: b['joints'][0].update(orientation=[0, 0, 0, 0]),
                     lambda b: b['joints'][0].update(orientation=[0, 0, 0, 2]),
                     lambda b: b['skeleton'][0].update(parent=256),
                     lambda b: b['skeleton'][0].update(parent=b['skeleton'][0]['joint']),
                     lambda b: b.update(all_joint_poses_tracked=False)]
        for mutate in mutations:
            body = self.sample()
            mutate(body)
            with self.assertRaises(ProtocolError):
                encode_command(dict(kind='body', observation=body))

    def test_malformed_wire_observation(self):
        outer = encode_command(dict(kind='body', observation=self.sample()))
        wrapper = decode_fields(outer.value)
        payload = decode_fields(wrapper[Tag.VALUES].value)
        baseline = decode_fields(payload[Tag.BODY_OBSERVATION].value)
        for mutation in ('width', 'unknown', 'missing_flags', 'order', 'duplicate', 'cycle'):
            fields = dict(baseline)
            if mutation == 'width':
                fields[2] = Field.u32(2, 1, True)
            elif mutation == 'unknown':
                fields[11] = Field.u32(11, 0, True)
            else:
                tag = 10 if mutation == 'cycle' else 9
                entries = list(decode_fields(fields[tag].value).values())
                if mutation == 'order':
                    entries.reverse()
                elif mutation == 'duplicate':
                    entries.append(entries[0])
                else:
                    parts = decode_fields(entries[0].value)
                    if mutation == 'missing_flags':
                        del parts[Tag.USAGE]
                    else:
                        parts[Tag.CHANNEL] = Field.u32(Tag.CHANNEL, entries[0].tag - 1, True)
                    entries[0] = Field.nested(entries[0].tag, list(parts.values()), True)
                if mutation == 'duplicate':
                    # Construct invalid bytes directly: encode_fields also rejects duplicate tags.
                    fields[tag] = Field.binary(tag, fields[tag].value + encode_fields([entries[0]]), True)
                else:
                    fields[tag] = Field.nested(tag, entries, True)
            changed = dict(payload)
            changed[Tag.BODY_OBSERVATION] = Field.nested(Tag.BODY_OBSERVATION, list(fields.values()), True)
            envelope = dict(wrapper)
            envelope[Tag.VALUES] = Field.nested(Tag.VALUES, list(changed.values()), True)
            record = dict(encoding='simxr-command-v1-hex', data=encode_fields(envelope.values()).hex())
            with self.assertRaises(ProtocolError):
                decode_command(record)

    def test_native_wire_fixtures(self):
        if NATIVE_CODEC is None:
            self.skipTest('Native codec executable not supplied')
        rows = subprocess.check_output([NATIVE_CODEC, '--python-fixtures'], text=True).splitlines()
        self.assertEqual(len(rows), 4)
        for joint_set, data in enumerate(rows):
            decoded = decode_command(dict(encoding='simxr-command-v1-hex', data=data))
            body = decoded['observation']
            self.assertEqual(body['joint_set'], joint_set)
            self.assertEqual(body['requested_time'], 2**53 + 1)
            self.assertEqual(decoded['observation_origin'], 2**53 + 1)
            self.assertEqual({j['joint'] for j in body['joints']}, JOINT_SETS[joint_set])
            self.assertEqual(encode_command(decoded).value.hex(), data)


class Pages:
    def __init__(self, pages):
        self.pages = pages
        self.requests = []

    def read_recording(self, session, *, offset, count):
        self.requests.append((session, offset, count))
        return self.pages[len(self.requests) - 1]


class RecordingTests(unittest.TestCase):
    def test_live_input_remains_editable_with_observed_edge_timestamps(self):
        event = outcome(identity=0, commands=[{'kind': 'controller_controls', 'primary': True}])
        event['after_frame'] = False
        event['source']['kind'] = 4
        event['live_input'] = dict(connection=23, sequence=7, observed_time=100, target_time=110,
                                   controls_observed_time=[90, 100], clock_revision=19, trackers_observed_time=80)
        document = recording([event], version=2)
        script = to_luau(document)
        self.assertIn('-- Tracker observation runtime ns: 80', script)
        self.assertIn('["primary"] = true', script)
        self.assertIn('-- Original frame 11; transaction 0; after_frame false', script)
        self.assertIn('-- Physical input: connection 23; sequence 7; clock_revision 19', script)
        self.assertIn('-- Original runtime ns: observed 100; target 110; controls 90, 100', script)
        self.assertIn('simxr.frame_add(baseFrame, "1")', script)
        self.assertEqual(document['transactions'][0]['live_input']['controls_observed_time'], [90, 100])
        for field, value in (('connection', 0), ('target_time', 99), ('clock_revision', -1), ('trackers_observed_time', 0), ('trackers_observed_time', 101), ('controls_observed_time', [101, 100])):
            invalid = deepcopy(document)
            invalid['transactions'][0]['live_input'][field] = value
            with self.assertRaises(ProtocolError):
                to_luau(invalid)

    def test_source_release_timing_and_validation(self):
        release = outcome(identity=0, frame=11, phase='released',
                          commands=[{'kind': 'release_control', 'channel': 'left_controls'}])
        release['after_frame'] = True
        first = dict(outcome(frame=11), after_frame=False)
        last = dict(outcome(2, frame=13), after_frame=False)
        document = recording([first, release, last], version=2)
        script = to_luau(document)
        self.assertLess(script.index('recorded frame +1'), script.index('recorded frame +2'))
        self.assertLess(script.index('recorded frame +2'), script.index('release_control'))
        self.assertLess(script.index('release_control'), script.index('recorded frame +3'))
        # A release at the origin still belongs before the first following frame.
        release['applied_frame'] = 10
        self.assertIn('recorded frame +1', to_luau(recording([release], version=2)))
        for changes in ({'transaction_id': 1}, {'after_frame': 1}, {'after_frame': None},
                        {'commands': first['commands']}, {'after_frame': False}, {'commands': []}):
            with self.subTest(changes=changes), self.assertRaises(ProtocolError):
                to_luau(recording([dict(release, **changes)], version=2))
        with self.assertRaises(ProtocolError):
            to_luau(recording([release], version=1))
        with self.assertRaises(ProtocolError):
            to_luau(recording([dict(release, applied_frame=U64_MAX)], version=2, origin_frame=0))
        # In-boundary cleanup follows the other commands on the same frame.
        release.update(applied_frame=11, after_frame=False)
        script = to_luau(recording([first, release], version=2))
        self.assertEqual(script.count('do -- recorded frame +1'), 1)
        self.assertLess(script.index('gaze', script.index('do --')), script.index('release_control'))

    def test_reference_space_origin_is_exact_and_required(self):
        origin = {'kind': 'reference_spaces', 'local_position': [3, 1.75, -2],
                  'local_orientation': [0, 0, 0, 1], 'stage_position': [3, 0, -2],
                  'stage_orientation': [0, 0, 0, 1], 'bounds_available': False,
                  'stage_bounds': [0, 0]}
        command = encoded(origin)
        self.assertEqual(decode_command(command), origin)
        script = to_luau(recording(initial_commands=[command]))
        self.assertIn('reference_spaces', script)
        self.assertIn('bounds_available', script)
        for origins in ([], [command, command]):
            with self.assertRaises(ProtocolError):
                to_luau(recording(initial_commands=origins))
        with self.assertRaises(ProtocolError):
            encode_command(dict(origin, stage_bounds=[2, 2]))

    def test_exact_frame_helpers_preserve_full_uint64_offsets_and_seed(self):
        relative = 2**53 + 3
        source = recording([outcome(frame=relative + 1)], origin_frame=1)
        script = to_luau(source)
        self.assertIn('local baseFrame = simxr.frame_add(simxr.state().frame_id or "0", "1")', script)
        self.assertIn('simxr.frame_before(simxr.state().frame_id or "0", target)', script)
        self.assertIn(f'local target = simxr.frame_add(baseFrame, "{relative}")', script)
        self.assertIn(f'assert(simxr.seed == "{U64_MAX}"', script)
        self.assertNotIn('tonumber', script)
        self.assertNotIn('baseFrame +', script)
        largest = to_luau(recording([outcome(frame=U64_MAX)], origin_frame=0))
        self.assertIn(f'simxr.frame_add(baseFrame, "{U64_MAX}")', largest)

    def test_every_app_step_drains_actual_display_and_releases_both_captures(self):
        source = recording([outcome(index, 10 + index) for index in range(1, 21)])
        script = to_luau(source)
        self.assertIn('local capture, progress, sourceFrame = simxr.step_display()', script)
        self.assertIn('until sourceFrame and not simxr.frame_before(sourceFrame, appliedFrame)', script)
        self.assertIn('local appliedFrame = simxr.state().frame_id or "0"', script)
        app_step = script.index('local capture = simxr.step()')
        app_release = script.index('if capture then simxr.release_capture(capture) end', app_step)
        display_drain = script.index('presentThrough(appliedFrame)', app_step)
        self.assertLess(app_release, display_drain)
        display_step = script.index('simxr.step_display()')
        display_release = script.index('if capture then simxr.release_capture(capture) end', display_step)
        self.assertLess(display_release, script.index('until sourceFrame'))
        self.assertEqual(script.count('table.insert(pending'), 20)

    def test_all_command_variants_remain_named_editable_and_lossless(self):
        commands = [
            {'kind': 'take_control', 'channel': 'head', 'replace': True},
            {'kind': 'release_control', 'channel': 'gaze'},
            {'kind': 'head_pose', 'orientation': [0, 1, 0, 0], 'position': [1, 2, 3], 'flags': 3},
            {'kind': 'controller_pose', 'hand': 1, 'position': [-1, 2, -3], 'connected': False},
            {'kind': 'controller_pose', 'hand': 0, 'flags': 0,
             'aim': {'position': [3, 4, 5], 'orientation': [0, 1, 0, 0], 'flags': 15}},
            {'kind': 'controller_pose', 'hand': 0, 'aim': {'flags': 0}},
            {'kind': 'controller_controls', 'hand': 1, 'trigger': .75, 'trackpad_force': .25,
             'primary': True, 'thumbrest_touch': True, 'connected': False},
            {'kind': 'body'},
            {'kind': 'body', 'preset': 1, 'active': False, 'confidence': .25,
             'root_pose': {'position': [3, 2, 1], 'orientation': [0, 1, 0, 0]},
             'controls': [{'tracked': False, 'offset': [1, 2, 3]}] + [{} for _ in range(4)],
             'joints': [{'joint': 83, 'tracked': False, 'position': [4, 5, 6]}]},
            {'kind': 'gaze', 'yaw': .25, 'pitch': -.5, 'available': False, 'tracked': False},
            {'kind': 'tracker', 'index': 3, 'identity': U64_MAX, 'role': 17, 'anchor': 83,
             'position': [7, 8, 9], 'tracked': False, 'connected': False},
            {'kind': 'views', 'ipd': .0625, 'inset_half_angle': .25,
             'outer_fov': [-1, .5, .75, -.25] * 2, 'outer_width': 4096, 'inset_height': 123},
            {'kind': 'proportions', 'height': 2, 'shoulder_width': .5, 'hip_width': .25},
            {'kind': 'controller_profile', 'hand': 1, 'path': '/interaction_profiles/vendor/測試'},
            {'kind': 'scenario', 'scenario': 'head_untracked', 'enabled': True},
        ]
        document = recording([outcome(commands=commands)])
        before = deepcopy(document)
        script = to_luau(document)
        self.assertEqual(document, before)
        for command in commands:
            canonical = encoded(command)
            decoded = decode_command(canonical)
            self.assertEqual(encode_command(decoded).value.hex(), canonical['data'])
            self.assertIn(implementation._luau(decoded), script)
            if command['kind'] == 'body':
                self.assertEqual(decoded.get('root_pose'), command.get('root_pose'))
        self.assertIn(f'["identity"] = "{U64_MAX}"', script)
        self.assertIn('測試', script)
        self.assertNotIn('simxr-command-v1-hex', script)
        self.assertIn('["joint"] = 83', script)
        self.assertIn('["thumbrest_touch"] = true', script)

    def test_groups_ordered_frames_without_reordering_same_frame_commands(self):
        source = recording([outcome(1, 14), outcome(2, 12), outcome(3, 12)])
        script = to_luau(source)
        self.assertLess(script.index('recorded frame +2'), script.index('recorded frame +4'))
        self.assertEqual(script.count('local target = simxr.frame_add'), 2)
        self.assertEqual(script.count('table.insert(pending'), 3)

    def test_rejected_outcomes_remain_diagnostics_not_replayed(self):
        source = recording([outcome(phase='conflict', frame=0)])
        script = to_luau(source)
        self.assertIn('-- Observed transaction 1: conflict', script)
        self.assertNotIn('table.insert(pending', script)

    def test_empty_recording_still_restores_initial_configuration(self):
        source = recording([])
        script = to_luau(source)
        self.assertIn('head_pose', script)
        self.assertIn('take_control', script)
        self.assertNotIn('local target', script)
        self.assertEqual(collect_recording(Pages([page(source)]), SESSION), source)

    def test_pages_preserve_all_fields_and_own_copied_values(self):
        first = recording([outcome(1)], total=2)
        second = recording([outcome(2)], total=2)
        client = Pages([page(first, end=False), page(second, offset=1)])
        collected = collect_recording(client, SESSION)
        self.assertEqual(client.requests, [(SESSION, 0, 16), (SESSION, 1, 16)])
        self.assertEqual(collected['transactions'], first['transactions'] + second['transactions'])
        self.assertEqual(collected['initial'], first['initial'])
        self.assertEqual(collected['transactions'][0]['source']['id'], U64_MAX)
        first['transactions'][0]['source']['id'] = 0
        second['transactions'][0]['commands'].clear()
        first['initial']['head']['position'][0] = 99
        self.assertEqual(collected['transactions'][0]['source']['id'], U64_MAX)
        self.assertTrue(collected['transactions'][1]['commands'])
        self.assertEqual(collected['initial']['head']['position'][0], 1)

    def test_stable_recording_identity_includes_initial_state_and_commands(self):
        mutations = {'version': 2, 'seed': 7, 'origin_frame': 9, 'origin_time': 1,
                     'total': 3, 'initial': {'changed': True},
                     'initial_commands': [encoded({'kind': 'gaze'})], 'initial_scenarios': [0]}
        for key, changed in mutations.items():
            with self.subTest(field=key):
                first = recording([outcome()], total=2)
                second = recording([outcome(2)], total=2)
                second[key] = changed
                with self.assertRaises(ProtocolError):
                    collect_recording(Pages([page(first, end=False), page(second, offset=1)]), SESSION)

    def test_session_and_generation_are_bound_on_every_page(self):
        for identity in ({'session': 99}, {'generation': 24}, {'generation': True}):
            with self.subTest(identity=identity), self.assertRaises(ProtocolError):
                collect_recording(Pages([page(recording([outcome()], total=2), end=False),
                                        page(recording([outcome(2)], total=2), offset=1, **identity)]), SESSION)

    def test_page_length_offset_end_and_progress_are_strict(self):
        cases = [page(recording(), length=0), page(recording(), offset=1),
                 page(recording(), end=False), page(recording(), end=1),
                 page(recording([], total=2), end=False),
                 page(recording([outcome()], total=2), end=True),
                 page(recording([outcome()] * 17))]
        for index, value in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(ProtocolError):
                collect_recording(Pages([value]), SESSION)

    def test_document_bounds_and_integer_types_are_strict(self):
        changes = [{'active': True}, {'active': 0}, {'overflow': True}, {'overflow': 0},
                   {'total': 1025}, {'total': True}, {'seed': float(2**53)},
                   {'seed': -1}, {'seed': U64_MAX + 1}, {'origin_frame': 1.0},
                   {'origin_time': 2**63}, {'initial': None}]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ProtocolError):
                to_luau(recording(**change))
        source = recording()
        source['transactions'] = None
        with self.assertRaises(ProtocolError):
            to_luau(source)
        with self.assertRaises(ProtocolError):
            to_luau(recording([outcome()], total=2))
        with self.assertRaises(ProtocolError):
            to_luau(recording([outcome(frame=10)]))
        with self.assertRaises(ProtocolError):
            to_luau(recording([outcome(phase='applied\nsimxr.resume()')]))
        with self.assertRaises(ProtocolError):
            to_luau(recording([outcome(identity=True)]))
        with self.assertRaises(ProtocolError):
            to_luau(recording([outcome(phase=[])]))

    def test_malformed_and_unrepresentable_commands_fail_instead_of_losing_fields(self):
        for data in ('0', 'zz', '', 'aa bb', '00' * (64 * 1024 + 1)):
            source = recording()
            source['initial_commands'][0]['data'] = data
            with self.subTest(data=data[:12]), self.assertRaises(ProtocolError):
                to_luau(source)
        source = recording()
        source['initial_commands'] = [{'encoding': 'simxr-command-v1-hex'}]
        with self.assertRaises(ProtocolError):
            to_luau(source)
        canonical = bytes.fromhex(encoded({'kind': 'gaze'})['data'])
        fields = decode_fields(canonical)
        fields[Tag.VALUES] = Field.nested(Tag.VALUES, [
            *decode_fields(fields[Tag.VALUES].value).values(), Field.u32(65000, 1)], True)
        source = recording()
        source['initial_commands'][0]['data'] = encode_fields(list(fields.values())).hex()
        with self.assertRaises(ProtocolError):
            to_luau(source)

    def test_command_count_collection_bytes_and_runner_source_are_bounded(self):
        source = recording()
        source['initial_commands'] *= 65
        with self.assertRaises(ProtocolError):
            to_luau(source)
        with patch.object(implementation, '_MAX_RECORDING_BYTES', 1), self.assertRaises(ProtocolError):
            collect_recording(Pages([page(recording())]), SESSION)
        with patch.object(implementation, '_MAX_RIG_SOURCE_BYTES', 1), self.assertRaises(ProtocolError):
            to_luau(recording())

    def test_initial_scenarios_restore_persistent_state_and_clear_other_failures(self):
        source = recording(initial_scenarios=[int(value) for value in (Scenario.HEAD_UNTRACKED, Scenario.UNFOCUSED, Scenario.RECENTER)])
        script = to_luau(source)
        for scenario in Scenario:
            enabled = scenario in (Scenario.HEAD_UNTRACKED, Scenario.UNFOCUSED)
            command = {'kind': 'scenario', 'scenario': scenario.name.lower(), 'enabled': enabled}
            self.assertIn(implementation._luau(command), script)
        self.assertIn('["scenario"] = "recenter", ["enabled"] = false', script)
        source['initial_scenarios'] = [int(Scenario.SESSION_LOSS)]
        with self.assertRaisesRegex(ProtocolError, 'after session loss'):
            to_luau(source)
        for scenarios in (None, [True], [13], [0, 0]):
            with self.subTest(scenarios=scenarios), self.assertRaises(ProtocolError):
                to_luau(recording(initial_scenarios=scenarios))
        for scenario in ('recenter', 'session_loss'):
            with self.subTest(trigger=scenario), self.assertRaises(ProtocolError):
                to_luau(recording(initial_commands=[encoded({'kind': 'scenario', 'scenario': scenario})]))
        source = recording()
        del source['initial_scenarios']
        with self.assertRaises(ProtocolError):
            to_luau(source)

    def test_luau_strings_use_valid_control_escapes(self):
        self.assertEqual(implementation._luau('A\n1\t\x7f"\\測試'), '"A\\0101\\009\\127\\"\\\\測試"')
        source = recording(initial={'bad': float('nan')})
        with self.assertRaises(ProtocolError):
            to_luau(source)


NATIVE_CODEC = None
if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--native-codec':
        NATIVE_CODEC = sys.argv[2]
        sys.argv = sys.argv[:1]
    unittest.main()
