"""Acknowledged sequence upload/execution through the real CPU control endpoint."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
from simxr_control import Client, Capability, ControlError
from simxr_control.protocol import Field, Tag, OpCode
HOST = sys.argv[1]


def wait_for(read, check):
    deadline = time.monotonic() + 5
    while True:
        value = read()
        if check(value):
            return value
        if time.monotonic() >= deadline:
            raise AssertionError(f'Sequence condition timed out: {value}')
        time.sleep(.005)


class SequenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='sx-seq-')
        self.path = Path(self.directory.name)
        self.socket, self.start, self.finish = [self.path / name for name in ('control.sock', 'start', 'finish')]
        self.process = subprocess.Popen([HOST, str(self.socket), str(self.start), str(self.finish)],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        wait_for(self.socket.exists, bool)
        self.client = Client(self.socket, required=Capability.POSE_SEQUENCES)
        self.session = self.client.sessions()[0]

    def tearDown(self):
        self.client.close()
        self.start.touch()
        self.finish.touch()
        try:
            output, _ = self.process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            output, _ = self.process.communicate()
        self.directory.cleanup()
        self.assertEqual(self.process.returncode, 0, output)

    def upload_chunk(self, program, offset, total, data):
        return self.client.request(OpCode.UPLOAD_SEQUENCE, self.client._session(self.session) + [
            Field.u64(Tag.PROGRAM_ID, program, True), Field.u64(Tag.OFFSET, offset, True),
            Field.u64(Tag.TOTAL_BYTES, total, True), Field.binary(Tag.BYTES, data, True)])

    def test_stale_generation_and_source_identity(self):
        program = self.client.upload_sequence(self.session, b'{"frame":0,"capture":true}\n')
        stale = type(self.session)(self.session.id, self.session.generation + 1)
        with self.assertRaises(ControlError) as error:
            self.client.start_sequence(stale, program)
        self.assertEqual(error.exception.code.name, 'STALE_GENERATION')
        for source in (2, 3):
            with self.assertRaises(ControlError):
                self.client.start_sequence(self.session, program, source=source)
        sequence = self.client.start_sequence(self.session, program)['sequence_id']
        self.assertEqual(self.client.sequence_status(self.session, sequence)['document']['status']['origin'], 0)
        empty = self.client.upload_sequence(self.session, b'')
        self.assertNotEqual(self.client.start_sequence(self.session, empty)['sequence_id'], sequence)

    def test_shared_source_recording_sequence_disconnect(self):
        recording = self.client.start_recording(self.session, drain=True)['document']
        program = self.client.upload_sequence(self.session, '{"frame":0,"head":{"x":7},"capture":true}\n')
        sequence = self.client.start_sequence(self.session, program, loop=True)['sequence_id']
        self.start.touch()
        page = wait_for(lambda: self.client.sequence_status(self.session, sequence)['document'],
                        lambda state: state['status']['last_frame'] == 6)
        self.assertTrue(page['status']['active'])
        self.assertEqual(len(page['held_captures']), 6)
        identity = recording['recording_id']
        self.client.close()
        with Client(self.socket) as successor:
            wait_for(lambda: successor.read_recording(self.session, recording_id=identity)['document'],
                     lambda state: not state['active'])
            next_program = successor.upload_sequence(self.session, '{"frame":0}\n')
            next_sequence = successor.start_sequence(self.session, next_program)['sequence_id']
            self.assertNotEqual(next_sequence, sequence)
            next_recording = successor.start_recording(self.session, drain=True)['document']
            self.assertNotEqual(next_recording['recording_id'], identity)
            with self.assertRaises(ControlError):
                successor.stop_recording(self.session, recording_id=identity)
            successor.release_control(self.session)
            self.assertFalse(successor.read_recording(self.session,
                recording_id=next_recording['recording_id'])['document']['active'])
            self.assertFalse(successor.sequence_status(self.session, next_sequence)['document']['status']['active'])

    def test_application_selector_validation(self):
        fields = self.client._session(self.session) + [Field.u64(Tag.SEQUENCE_ID, 1, True)]
        for selector in (Field.u32(Tag.APPLICATION_OWNED, 1, True),
                         Field.binary(Tag.APPLICATION_OWNED, b'\x02', True)):
            with self.assertRaises(ControlError):
                self.client.request(OpCode.READ_SEQUENCE, fields + [selector])
        with self.assertRaises(ControlError):
            self.client.request(OpCode.READ_SEQUENCE, fields + [
                Field.boolean(Tag.APPLICATION_OWNED, True, True), Field.u32(Tag.SOURCE_KIND, 1)])
        program = self.client.upload_sequence(self.session, '{"frame":0,"capture":true}\n')
        sequence = self.client.start_sequence(self.session, program)['sequence_id']
        hidden = self.client.sequence_status(self.session, sequence, application_owned=True)['document']
        self.assertIsNone(hidden['status'])
        with self.assertRaises(ControlError):
            self.client.stop_sequence(self.session, sequence, application_owned=True)
        self.start.touch()
        page = wait_for(lambda: self.client.sequence_status(self.session, sequence)['document'],
                        lambda state: bool(state['captures']))
        with self.assertRaises(ControlError) as error:
            self.client.release_sequence_capture(self.session, page['captures'][0]['capture_id'],
                                                 application_owned=True)
        self.assertEqual(error.exception.code.name, 'CONFLICT')

    def test_chunks_retry_replacement_and_retirement(self):
        data = b'{"frame":0,"head":{"x":4},"capture":true}\n'
        first = self.upload_chunk(2**53 + 9, 0, len(data), data[:11])['document']
        self.assertEqual(first['received'], 11)
        self.assertFalse(first['complete'])
        with self.assertRaises(ControlError):
            self.client.start_sequence(self.session, 2**53 + 9)
        self.assertEqual(self.upload_chunk(2**53 + 9, 0, len(data), data[:11])['document'], first)
        for offset, chunk in [(12, data[12:]), (0, b'wrong')]:
            with self.assertRaises(ControlError) as error:
                self.upload_chunk(2**53 + 9, offset, len(data), chunk)
            self.assertEqual(error.exception.code.name, 'CONFLICT')
        self.upload_chunk(2**53 + 9, 11, len(data), data[11:])
        execution = self.client.start_sequence(self.session, 2**53 + 9, loop=True)
        sequence = execution['sequence_id']
        self.assertEqual(self.client.start_sequence(self.session, 2**53 + 9, loop=True)['sequence_id'], sequence)
        with self.assertRaises(ControlError):
            self.client.start_sequence(self.session, 2**53 + 9, loop=False)
        with Client(self.socket) as other:
            foreign = other.upload_sequence(self.session, data)
            with self.assertRaises(ControlError) as error:
                other.start_sequence(self.session, foreign)
            self.assertEqual(error.exception.code.name, 'BUSY')
        replacement = self.client.upload_sequence(self.session,
            b'{"frame":0,"head":{"x":7},"capture":true}\n', program_id=2**53 + 10)
        newer = self.client.start_sequence(self.session, replacement, loop=True)['sequence_id']
        self.assertNotEqual(sequence, newer)
        with self.assertRaises(ControlError):
            self.client.stop_sequence(self.session, sequence)
        self.start.touch()
        page = wait_for(lambda: self.client.sequence_status(self.session, newer)['document'],
                        lambda state: state['status']['last_frame'] == 6)
        self.assertFalse(page['history_gap'])
        self.assertEqual(len(page['captures']), 6)
        self.assertEqual([c['frame'] for c in page['captures']], list(range(1, 7)))
        self.assertTrue(all(c['admission'] == 'accepted' and c['phase'] == 'clear' for c in page['captures']))
        self.assertEqual(len(page['held_captures']), 6)
        self.assertEqual(self.client.simulation_status(self.session)['document']['configuration']['head']['position'][0], 7)
        status = self.client.status(self.session)
        self.assertFalse(status['pipeline']['paused'])
        with Client(self.socket) as other:
            hidden = other.sequence_status(self.session, newer)['document']
            self.assertIsNone(hidden['status'])
            self.assertEqual(hidden['captures'], [])
            with self.assertRaises(ControlError) as error:
                other.release_sequence_capture(self.session, page['captures'][0]['capture_id'])
            self.assertEqual(error.exception.code.name, 'CONFLICT')
        for capture in page['captures']:
            self.assertTrue(self.client.release_sequence_capture(self.session, capture['capture_id'])['released'])
        stopped = self.client.stop_sequence(self.session, newer)['document']
        self.assertEqual(stopped['result'], 'cancelled')
        self.assertEqual(self.client.start_sequence(self.session, replacement, loop=True)['document']['result'], 'cancelled')
        self.assertEqual(self.client.sequence_status(self.session, newer)['document']['held_captures'], [])

    def test_large_program_limits_and_invalid_replacement(self):
        row = b'{"frame":0,"head":{"x":3}}\n'
        payload = row + b'#' + b' ' * (16 * 1024 * 1024 - len(row) - 1)
        program = self.client.upload_sequence(self.session, payload)
        current = self.client.start_sequence(self.session, program)['sequence_id']
        maximum = self.client.upload_sequence(self.session, row + b'{"frame":0}\n' * 99999)
        current = self.client.start_sequence(self.session, maximum)['sequence_id']
        oversized = self.client.upload_sequence(self.session, b'{"frame":0}\n' * 100001)
        with self.assertRaises(ControlError) as error:
            self.client.start_sequence(self.session, oversized)
        self.assertEqual(error.exception.code.name, 'CAPACITY')
        self.assertEqual(self.client.sequence_status(self.session)['document']['status']['id'], current)
        overflow = self.client.upload_sequence(self.session, '{"frame":"18446744073709551615"}\n')
        with self.assertRaises(ControlError) as error:
            self.client.start_sequence(self.session, overflow)
        self.assertEqual(error.exception.code.name, 'INVALID')
        self.assertEqual(self.client.sequence_status(self.session)['document']['status']['id'], current)
        with self.assertRaises(ControlError):
            self.upload_chunk(100, 0, 16 * 1024 * 1024 + 1, b'x')
        with self.assertRaises(ControlError):
            self.upload_chunk(101, 0, 1, b'\x00')
        self.start.touch()
        wait_for(lambda: self.client.sequence_status(self.session)['document']['status'], lambda s: not s['active'])
        self.assertEqual(self.client.simulation_status(self.session)['document']['configuration']['head']['position'][0], 3)
        self.client.close()
        with Client(self.socket) as observer:
            pending = observer.upload_sequence(self.session, '{"frame":0}\n')
            def try_start():
                try:
                    return observer.start_sequence(self.session, pending)
                except ControlError as error:
                    if error.code.name != 'BUSY':
                        raise
                    return None
            wait_for(try_start, bool)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
