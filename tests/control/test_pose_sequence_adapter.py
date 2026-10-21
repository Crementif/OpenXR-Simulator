"""Failure, ownership and bounded artifact behavior of the public sequence adapter."""
from copy import deepcopy
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
sys.path.insert(0, str(ROOT / 'mcp-server'))
from legacy_control import LegacyControl
from pose_sequence import CapturePersistenceError
from simxr_control import Capability, ProtocolError
from test_legacy_control import Endpoint


class SequenceEndpoint(Endpoint):
    def __init__(self):
        super().__init__()
        self.hello['capabilities'] |= Capability.POSE_SEQUENCES
        self.uploads = []
        self.execution = 2**53 + 10
        self.sequence_state = None
        self.capture_rows = []
        self.held = {}
        self.stops = []
        self.released = []
        self.readers_released = []
        self.gap = False
        self.malformed = False

    def upload_sequence(self, session, program):
        self.uploads.append(program)
        return len(self.uploads)

    def start_sequence(self, session, program, *, loop=False):
        self.execution += 1
        self.sequence_state = dict(id=self.execution, active=True, loop=loop, result='queued',
                                   origin=0, last_frame=0, groups=0)
        return {} if self.malformed else {'sequence_id': self.execution, 'document': deepcopy(self.sequence_state)}

    def sequence_status(self, session, sequence, *, after=0):
        rows = [row for row in self.capture_rows if row['frame'] > after]
        return {'document': {'status': deepcopy(self.sequence_state) if sequence == self.execution else None,
                             'next_frame': max([after] + [row['frame'] for row in rows]), 'more': False,
                             'history_gap': self.gap, 'captures': deepcopy(rows),
                             'held_captures': [{'capture_id': identity, 'frame': row['frame']}
                                              for identity, row in self.held.items()]}}

    def stop_sequence(self, session, sequence):
        self.stops.append(sequence)
        self.sequence_state.update(active=False, result='cancelled')
        return {'document': deepcopy(self.sequence_state)}

    def release_sequence_capture(self, session, identity):
        self.released.append(identity)
        return {'released': self.held.pop(identity, None) is not None}

    def capture(self, session, identity):
        return {'phase': self.held[identity].get('phase', 'clear'), 'result': 0}

    def retain_image(self, session, identity):
        return {'image_reader_id': 19}

    def image_info(self, reader):
        return {'frame': {'generation': self.current_session.generation, 'frame_id': 9}, 'image_descriptor': {}}

    def release_image(self, reader):
        self.readers_released.append(reader)


class SequenceAdapterTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='sx-seq-adapter-')
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'program.jsonl'
        self.path.write_text('{"frame":0,"head":{"x":3}}\n')
        self.endpoint = SequenceEndpoint()
        patch('simxr_control.discover', return_value=['chosen.sock']).start()
        patch('simxr_control.Client', return_value=self.endpoint).start()
        self.thread_patch = patch('pose_sequence.threading.Thread')
        self.thread = self.thread_patch.start()
        self.addCleanup(patch.stopall)
        self.control = LegacyControl(self.directory.name)
        self.addCleanup(self.control.close)

    def start(self, **arguments):
        return self.control.call('run_pose_sequence', {'path': str(self.path), **arguments})

    def test_invalid_input_preserves_playback_and_exact_replacement_receipt(self):
        started = self.start(loop=True)
        self.assertTrue(started['ok'], started)
        self.assertEqual(started['sequence_id'], str(2**53 + 11))
        self.assertFalse(self.start(loop=1)['ok'])
        self.assertFalse(self.start(path=str(self.path.with_suffix('.missing')))['ok'])
        self.assertEqual(self.endpoint.closed, 0)
        self.assertEqual(self.endpoint.execution, 2**53 + 11)
        if hasattr(os, 'mkfifo'):
            fifo = self.path.with_suffix('.fifo')
            os.mkfifo(fifo)
            self.assertFalse(self.start(path=str(fifo))['ok'])
        self.endpoint.held[27] = {'frame': 9}
        replacement = self.start()
        self.assertEqual(replacement['replacement']['captures_not_persisted'], [{'capture_id': '27', 'frame': '9'}])
        self.assertEqual(self.endpoint.released, [27])
        self.assertFalse(self.control.call('stop_pose_sequence', {'sequence_id': started['sequence_id']})['ok'])
        self.assertEqual(self.endpoint.stops, [])
        self.assertTrue(self.control.call('stop_pose_sequence', {'sequence_id': replacement['sequence_id']})['ok'])
        self.assertEqual(self.endpoint.stops, [2**53 + 12])

    def test_malformed_admission_retires_source_without_fallback_or_replay(self):
        self.endpoint.malformed = True
        result = self.start()
        self.assertFalse(result['ok'])
        self.assertEqual(result['transport'], 'pose_sequences')
        self.assertEqual(self.endpoint.closed, 1)
        self.assertFalse(self.start()['ok'])
        self.assertEqual(len(self.endpoint.uploads), 1)

    def test_history_gap_recovers_held_ticket_and_reports_missing_image(self):
        self.start()
        self.endpoint.gap = True
        self.endpoint.held[27] = {'frame': 9}
        status = self.control.call('get_pose_sequence', {})
        self.assertFalse(status['ok'])
        self.assertEqual(status['pending_captures'], 1)
        with self.control._lock:
            self.control._sequence._capture()
        status = self.control.call('get_pose_sequence', {})
        self.assertEqual(status['pending_captures'], 0)
        self.assertEqual(status['recent_captures'][0]['frame'], '9')
        self.assertEqual(status['recent_captures'][0]['phase'], 'clear')
        self.assertTrue(status['status']['active'])
        self.assertEqual(status['phase'], 'running')
        self.assertIn('no image', status['recent_captures'][0]['error'])
        self.assertEqual(self.endpoint.released, [27])

    def test_identity_mismatch_releases_ticket_and_reader_without_export(self):
        self.start()
        self.endpoint.held[27] = {'frame': 8, 'phase': 'ready'}
        self.control.call('get_pose_sequence', {})
        with patch('pose_sequence.read_stereo_image') as export, self.control._lock:
            with self.assertRaises(ProtocolError):
                self.control._sequence._capture()
        export.assert_not_called()
        self.assertEqual(self.endpoint.released, [27])
        self.assertEqual(self.endpoint.readers_released, [19])

    def test_worker_storage_failure_stops_playback_and_joins(self):
        self.thread_patch.stop()
        self.endpoint.held[27] = {'frame': 9}
        with patch('pose_sequence.PoseSequenceControl._record', side_effect=CapturePersistenceError('disk full')):
            started = self.start(loop=True)
            self.assertTrue(started['ok'], started)
            deadline = time.monotonic() + 2
            while True:
                status = self.control.call('get_pose_sequence', {})
                if status.get('phase') == 'failed':
                    break
                self.assertLess(time.monotonic(), deadline, status)
                time.sleep(.005)
        self.assertFalse(status['ok'])
        self.assertEqual(status['error'], 'disk full')
        self.assertEqual(status['failed_capture'], {'frame': '9', 'capture_id': '27'})
        self.assertFalse(status['status']['active'])
        self.assertEqual(status['pending_captures'], 0)
        self.assertEqual(self.endpoint.stops, [int(started['sequence_id'])])
        self.assertEqual(self.endpoint.released, [27])
        worker = self.control._sequence._worker
        self.control.close()
        self.assertFalse(worker.is_alive())
        self.assertEqual(self.endpoint.closed, 1)

    def test_absent_endpoint_only_falls_back_before_admission(self):
        with patch('simxr_control.discover', return_value=[]):
            self.assertIsNone(self.start())
        self.endpoint.hello['capabilities'] = Capability.SIMULATION_COMMANDS
        result = self.start()
        self.assertFalse(result['ok'])
        self.assertEqual(result['transport'], 'pose_sequences')
        self.assertEqual(self.endpoint.uploads, [])

    def test_disk_and_result_limits_remove_only_owned_files(self):
        self.start()
        owner = self.control._sequence
        owner.MAX_RESULTS, owner.MAX_DISK = 3, 10
        unrelated = owner._output / 'user.txt'
        unrelated.write_text('preserve')
        with self.control._lock:
            for frame in range(5):
                owner._record({'frame': frame, 'capture_id': frame + 1, 'phase': 'ready'}, png=b'png123')
        saved = list(owner._output.glob('*.png'))
        self.assertEqual(len(saved), 1)
        self.assertEqual(unrelated.read_text(), 'preserve')
        manifest = json.loads((owner._output / 'manifest.json').read_text())
        self.assertEqual(manifest['captures_saved'], 5)
        self.assertEqual(manifest['evicted_results'], 4)
        self.assertEqual(manifest['recent_captures'][0]['frame'], '4')
        self.assertEqual(list(owner._output.glob('*.tmp')), [])

    def test_capture_failure_records_once_and_releases_independent_reader(self):
        self.start()
        self.endpoint.held[27] = {'frame': 9, 'phase': 'ready'}
        self.control.call('get_pose_sequence', {})
        with patch('pose_sequence.read_stereo_image', side_effect=ValueError('unsupported pixels')), self.control._lock:
            self.control._sequence._capture()
        status = self.control.call('get_pose_sequence', {})
        self.assertEqual(status['captures_seen'], 1)
        self.assertEqual(status['captures_saved'], 0)
        self.assertFalse(status['ok'])
        self.assertEqual(status['recent_captures'][0]['error'], 'unsupported pixels')
        self.assertEqual(self.endpoint.readers_released, [19])


if __name__ == '__main__':
    unittest.main()
