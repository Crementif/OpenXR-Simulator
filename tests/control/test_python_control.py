import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import (Client, CommandKind, ControlError, ControlTimeout, ErrorCode,
                           ProtocolError, Session)
from simxr_control.protocol import Field, HEADER, Message, OpCode, Tag, decode_fields, encode_fields, to_dict

HOST = None


class FrameRecordingTests(unittest.TestCase):
    def test_recording_cleanup_and_export_bounds(self):
        from simxr_control import record_frames
        from simxr_control.protocol import Capability

        for failure in (None, 'stop', 'reader', 'chunk'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                client = Mock()
                client.status.return_value = {
                    'manual': False, 'capabilities': Capability.IMAGE_EXPORT | Capability.IMAGE_RECORDING,
                    'image_recording': {'phase': 'recording', 'captures': [1]}}
                client.acquire.return_value = 'lease'

                def submit(lease, kind, **arguments):
                    if failure == 'stop' and kind == CommandKind.STOP_IMAGE_RECORDING:
                        raise RuntimeError('stop failed')
                    return {'operation_id': 1}

                client.submit.side_effect = submit
                client.operation.return_value = {'phase': 'complete', 'command_id': 1, 'capture_id': 1}
                client.capture.return_value = {'phase': 'ready', 'capture_id': 1,
                                               'frame': {'frame_id': 1}, 'image_available': True}
                client.inspect.return_value = {'inspection_id': 2, 'document': {'frame_id': 1}}
                client.retain_image.return_value = {'image_reader_id': 3}
                client.image_info.return_value = {'image_descriptor': {'width': 1, 'height': 1, 'array_layers': 1}}
                client.start_export.return_value = {'export_id': 4}
                client.export_info.return_value = {'phase': 'complete', 'total_bytes': 4}
                client.read_export.return_value = {'offset': 0, 'bytes': b'12345' if failure == 'chunk' else b'1234'}
                if failure == 'reader':
                    client.release_image.side_effect = RuntimeError('reader release failed')
                directory = Path(temporary) / 'frames'
                if failure:
                    with self.assertRaises(RuntimeError):
                        record_frames(client, Session(1, 1), directory, 1, mode='realtime', max_frame_bytes=4)
                else:
                    result = record_frames(client, Session(1, 1), directory, 1, mode='realtime', max_frame_bytes=4)
                    self.assertEqual(result['phase'], 'complete')
                summary = json.loads((directory / 'recording.json').read_text())
                self.assertEqual(summary['phase'], 'failed' if failure else 'complete')
                self.assertEqual(summary['persisted'], 0 if failure == 'chunk' else 1)
                if failure:
                    self.assertIn('error', summary)
                client.release_export.assert_called_once_with(4)
                client.release_image.assert_called_once_with(3)
                client.release_inspection.assert_called_once_with(2)
                self.assertTrue(any(call.args[1] == CommandKind.RELEASE for call in client.submit.call_args_list))
                client.relinquish.assert_called_once_with('lease')
                if failure == 'chunk':
                    self.assertFalse(list(directory.glob('*.rgba')))
                else:
                    self.assertEqual(next(directory.glob('*.rgba')).read_bytes(), b'1234')


class ProtocolTests(unittest.TestCase):
    def test_unknown_phase_never_becomes_terminal_success(self):
        client = object.__new__(Client)
        client.operation = lambda *args: {'phase': 'future_pending_phase'}
        with self.assertRaises(ProtocolError):
            client.wait(Session(1, 1), 1)

    def test_unknown_simulation_phase_never_becomes_acknowledged(self):
        client = object.__new__(Client)
        client.simulation_status = lambda *args, **kwargs: {'phase': 'future_queued_phase'}
        with self.assertRaises(ProtocolError):
            client.wait_simulation(Session(1, 1), 1)

    def test_exact_width_and_nested_records(self):
        message = Message(OpCode.STATUS, 2**64 - 1,
                          (Field.u64(Tag.SESSION, 2**64 - 1), Field.i64(Tag.TIME, -2**63),
                           Field.text(Tag.DETAIL, '偏移 €'), Field.u64(Tag.IMAGE_TICK, 2**64 - 2), Field.nested(Tag.FRAME, [Field.u64(Tag.FRAME_ID, 7)])))
        encoded = message.encode()
        self.assertEqual(encoded[:4], b'SXR2')
        self.assertEqual(encoded[16:24], b'\xff' * 8)
        decoded = to_dict(Message.decode(encoded).fields)
        self.assertEqual(decoded['session'], 2**64 - 1)
        self.assertEqual(decoded['time'], -2**63)
        self.assertEqual(decoded['detail'], '偏移 €')
        self.assertEqual(decoded['frame']['frame_id'], 7)
        self.assertEqual(decoded['image_tick'], 2**64 - 2)

    def test_malformed_and_evolution(self):
        for packet in (b'', b'\1' * 31, Message(1, 1).encode() + b'\0'):
            with self.assertRaises(ProtocolError):
                Message.decode(packet)
        with self.assertRaises(ProtocolError):
            encode_fields([Field.u32(1, 1), Field.u32(1, 2)])
        for payload in (struct.pack('<HHI', 1, 2, 0), struct.pack('<HHI', 1, 0, 99)):
            with self.assertRaises(ProtocolError):
                decode_fields(payload)
        self.assertEqual(to_dict([Field(60000, b'optional')]), {})
        with self.assertRaises(ProtocolError):
            to_dict([Field(60000, b'required', True)])
        for value in (b'\xc0\x80', b'a\0b', b'\xed\xa0\x80'):
            with self.assertRaises(ProtocolError):
                Field(Tag.DETAIL, value).as_text()
        future = Message(1, 1, minor=100)
        self.assertEqual(Message.decode(future.encode()).minor, 100)

    def test_partial_timeout_preserves_single_exchange(self):
        with tempfile.TemporaryDirectory(prefix='simxr-sdk-') as temporary:
            path = str(Path(temporary) / 'control.sock')
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(path)
            os.chmod(path, 0o600)
            listener.listen(1)
            errors, requests = [], []

            def read(connection):
                data = bytearray()
                total = HEADER.size
                while len(data) < total:
                    data.extend(connection.recv(total - len(data)))
                    if len(data) == HEADER.size:
                        total = Message.size(data)
                return Message.decode(data)

            def serve():
                try:
                    with listener.accept()[0] as connection:
                        hello = read(connection)
                        connection.sendall(Message(hello.opcode, hello.request_id,
                                                   (Field.u32(Tag.ERROR, 0, True),), flags=1).encode())
                        submitted = read(connection)
                        requests.append(submitted)
                        packet = Message(submitted.opcode, submitted.request_id,
                                         (Field.u32(Tag.ERROR, 0, True), Field.u64(Tag.OPERATION_ID, 88)), flags=1).encode()
                        connection.sendall(packet[:HEADER.size + 2])
                        time.sleep(0.15)
                        connection.sendall(packet[HEADER.size + 2:])
                        connection.settimeout(0.1)
                        self.assertEqual(connection.recv(1), b'')
                except BaseException as error:
                    errors.append(error)

            thread = threading.Thread(target=serve)
            thread.start()
            try:
                with Client(path, timeout=0.05) as client:
                    with self.assertRaises(ControlTimeout):
                        client.request(OpCode.SUBMIT, [Field.u64(Tag.COMMAND_ID, 11)])
                    with self.assertRaises(ControlTimeout):
                        client.request(OpCode.SESSIONS)
                    time.sleep(0.17)
                    self.assertEqual(client.retry()['operation_id'], 88)
            finally:
                listener.close()
                thread.join(2)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(len(requests), 1)


@unittest.skipUnless(HOST, 'C++ endpoint host was not supplied')
class EndpointTests(unittest.TestCase):
    def test_fragmented_messages_and_shutdown(self):
        with tempfile.TemporaryDirectory(prefix='simxr-socket-') as temporary:
            path = Path(temporary) / 'control.sock'
            start = Path(temporary) / 'start'
            process = subprocess.Popen([HOST, str(path), str(start)], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            connections = []

            def connect():
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                connection.settimeout(2)
                connection.connect(str(path))
                connections.append(connection)
                return connection

            def receive(connection):
                data, total = bytearray(), HEADER.size
                while len(data) < total:
                    chunk = connection.recv(total - len(data))
                    self.assertTrue(chunk)
                    data.extend(chunk)
                    if len(data) == HEADER.size:
                        total = Message.size(data)
                return to_dict(Message.decode(data).fields)

            try:
                deadline = time.monotonic() + 5
                while not path.exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.005)
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                connection = connect()
                packet = Message(OpCode.HELLO, 1, (Field.text(Tag.CLIENT_NAME, 'fragmented €'),)).encode()
                for byte in packet:
                    connection.sendall(bytes([byte]))
                self.assertEqual(receive(connection)['error'], 0)
                connection.sendall(Message(65000, 2).encode())
                self.assertEqual(receive(connection)['error'], ErrorCode.UNSUPPORTED_COMMAND)
                connection.sendall(Message(OpCode.SESSIONS, 3, (Field(60000, b'optional'),)).encode())
                self.assertEqual(len(receive(connection)['sessions']), 1)
                connection.sendall(Message(OpCode.SESSIONS, 4, (Field(60000, b'critical', True),)).encode())
                self.assertEqual(receive(connection)['error'], ErrorCode.UNSUPPORTED_FIELD)
                version = connect()
                packet = bytearray(Message(OpCode.HELLO, 1).encode())
                packet[4:6] = struct.pack('<H', 99)
                version.sendall(packet)
                self.assertEqual(receive(version)['error'], ErrorCode.UNSUPPORTED_VERSION)
                self.assertEqual(version.recv(1), b'')
                oversized = connect()
                packet = bytearray(Message(OpCode.HELLO, 1).encode())
                packet[24:28] = struct.pack('<I', 1024 * 1024 + 1)
                oversized.sendall(packet)
                self.assertEqual(oversized.recv(1), b'')
                # Three workers blocked on different incomplete prefixes must join promptly.
                for prefix in (b'S', b'SXR2', Message(OpCode.HELLO, 1, (Field.text(Tag.CLIENT_NAME, 'body'),)).encode()[:35]):
                    connect().sendall(prefix)
                started = time.monotonic()
                start.touch()
                output = process.communicate(timeout=3)[0]
                self.assertEqual(process.returncode, 0, output)
                self.assertLess(time.monotonic() - started, 2)
                self.assertFalse(path.exists())
            finally:
                for connection in connections:
                    connection.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if process.stdout:
                    process.stdout.close()

    def test_real_service_from_python(self):
        with tempfile.TemporaryDirectory(prefix='simxr-control-') as temporary:
            path = Path(temporary) / 'control.sock'
            start = Path(temporary) / 'start'
            process = subprocess.Popen([HOST, str(path), str(start)], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.monotonic() + 5
                while not path.exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.005)
                with Client(path) as client, Client(path) as observer:
                    session, = client.sessions()
                    self.assertGreater(session.id, 0)
                    self.assertGreater(session.generation, 0)
                    lease = client.acquire(session)
                    with self.assertRaises(ControlError) as busy:
                        observer.acquire(session)
                    self.assertEqual(busy.exception.code, ErrorCode.BUSY)
                    with self.assertRaises(ControlError) as stale:
                        client.status(Session(session.id, session.generation + 1))
                    self.assertEqual(stale.exception.code, ErrorCode.STALE_GENERATION)
                    pause = client.submit(lease, CommandKind.PAUSE)['operation_id']
                    self.assertEqual(client.wait(session, pause)['progress'], 'paused')
                    self.assertTrue(observer.status(session)['manual'])
                    client.renew(lease)
                    retained = []
                    for index in range(1, 5):
                        command_id = 10 + index
                        step = client.submit(lease, CommandKind.STEP, command_id=command_id)['operation_id']
                        duplicate = client.submit(lease, CommandKind.STEP, command_id=command_id)
                        self.assertTrue(duplicate['duplicate'])
                        self.assertEqual(duplicate['operation_id'], step)
                        if index == 1:
                            start.touch()
                        ready = client.wait(session, step)
                        self.assertEqual(ready['phase'], 'complete')
                        self.assertEqual(ready['progress'], 'clear')
                        self.assertEqual(ready['frame']['frame_id'], index)
                        capture = client.capture(session, ready['capture_id'])
                        self.assertEqual(capture['phase'], 'clear')
                        self.assertEqual(capture['frame']['frame_id'], index)
                        self.assertEqual(capture['frame']['submission']['output_disposition'], 'clear')
                        inspection = client.inspect(session, ready['capture_id'])
                        document = inspection['document']
                        self.assertEqual(document['frame_id'], index)
                        self.assertEqual(document['generation'], session.generation)
                        self.assertEqual(document['submission']['output_disposition'], 'clear')
                        self.assertEqual(client.inspect(session, ready['capture_id'])['inspection_id'], inspection['inspection_id'])
                        revision_id = document['view_settings']['revision_id']
                        self.assertEqual(client.revision(revision_id)['revision_kind'], 'view_settings_v1')
                        self.assertEqual(observer.status(session)['pipeline']['permits'], 0)
                        # Connection-scoped IDs cannot reveal another client's revision.
                        with self.assertRaises(ControlError) as foreign:
                            observer.revision(revision_id)
                        self.assertEqual(foreign.exception.code, ErrorCode.NOT_FOUND)
                        self.assertTrue(client.release_inspection(inspection['inspection_id'])['released'])
                        with self.assertRaises(ControlError) as released_revision:
                            client.revision(revision_id)
                        self.assertEqual(released_revision.exception.code, ErrorCode.NOT_FOUND)
                        retained.append(ready['capture_id'])
                    # Status/polling and duplicate commands did not issue frame five.
                    self.assertEqual(client.status(session)['pipeline']['permits'], 0)
                    for capture in retained:
                        release = client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id']
                        self.assertEqual(client.wait(session, release)['progress'], 'released')
                        with self.assertRaises(ControlError) as absent:
                            client.capture(session, capture)
                        self.assertEqual(absent.exception.code, ErrorCode.NOT_FOUND)
                    client.acknowledge(lease, 14)
                    # Abrupt controller disconnect recovers admission without an XR call.
                    client.close()
                    output = process.communicate(timeout=5)[0]
                    self.assertEqual(process.returncode, 0, output)
                    self.assertIn('PASS CPU control endpoint lifecycle', output)
                self.assertFalse(path.exists())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if process.stdout:
                    process.stdout.close()


if __name__ == '__main__':
    if len(sys.argv) == 2 and not sys.argv[1].startswith('-'):
        HOST = sys.argv.pop(1)
        EndpointTests.__unittest_skip__ = False
    unittest.main()
