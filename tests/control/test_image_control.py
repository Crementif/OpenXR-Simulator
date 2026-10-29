"""Actual C++ endpoint + SDK FD tests using an explicitly CPU-only provider."""
import array
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Client, ControlError, ErrorCode
from simxr_control.protocol import Field, HEADER, Message, OpCode, Tag, to_dict

HOST = None


def packet_read(connection):
    packet = bytearray()
    descriptors = []
    total = HEADER.size
    try:
        while len(packet) < total:
            chunk, ancillary, flags, _ = connection.recvmsg(total - len(packet), socket.CMSG_SPACE(16), socket.MSG_CMSG_CLOEXEC)
            for level, kind, value in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    fds = array.array('i')
                    fds.frombytes(value)
                    descriptors.extend(fds)
                else:
                    raise AssertionError('Unexpected ancillary data')
            if flags & ~socket.MSG_CMSG_CLOEXEC:
                raise AssertionError('Truncated ancillary data')
            if not chunk:
                raise EOFError('Endpoint closed before response')
            packet.extend(chunk)
            if len(packet) == HEADER.size:
                total = Message.size(packet)
        return Message.decode(packet), descriptors
    except BaseException:
        for fd in descriptors:
            os.close(fd)
        raise


@unittest.skipUnless(HOST, 'C++ image endpoint host was not supplied')
class ImageEndpointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='simxr-image-sdk-')
        directory = Path(self.temporary.name)
        self.path, self.stop, self.ready, self.destroy = [directory / name for name in ('socket', 'stop', 'ready', 'destroy')]
        self.process = subprocess.Popen([HOST, str(self.path), str(self.stop), str(self.ready), str(self.destroy)],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.process.stdout.readline()
        self.assertTrue(line.startswith('READY CPU image fixture'), line)
        self.capture = int(line.split('capture=')[1].split(';')[0])
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            client.close()
        self.stop.touch()
        output, _ = self.process.communicate(timeout=5)
        self.assertEqual(self.process.returncode, 0, output)
        self.assertIn('PASS CPU image endpoint', output)
        self.temporary.cleanup()

    def client(self):
        result = Client(self.path)
        self.clients.append(result)
        return result

    def test_sdk_image_delivery_and_explicit_export_after_session_teardown(self):
        client = self.client()
        session, = client.sessions()
        retained = client.retain_image(session, self.capture)
        image = retained['image_reader_id']
        self.assertEqual(retained['phase'], 'complete')
        descriptor = retained['image_descriptor']
        self.assertEqual((descriptor['width'], descriptor['height'], descriptor['array_layers']), (4, 3, 2))
        self.assertTrue(descriptor['dedicated'])
        reader = os.open('/dev/null', os.O_RDONLY | os.O_CLOEXEC)
        try:
            shared = client.share_image(image, descriptor['device_uuid'], descriptor['driver_uuid'], reader)
            os.fstat(reader)  # Caller retains its original consumer descriptor.
        finally:
            os.close(reader)
        handles = shared['_handles']
        self.addCleanup(handles.close)
        self.assertEqual(len(handles), 2)
        self.assertFalse(os.get_inheritable(handles[0]))
        self.assertFalse(os.get_inheritable(handles[1]))
        expected = bytes((index * 17 + 3) & 255 for index in range(96))
        self.assertEqual(os.pread(handles[0], 96, 0), expected)
        self.assertEqual(os.read(handles[1], 1), b'\1')  # CPU fixture pipe, not a GPU semaphore.
        delivery = shared['delivery_id']
        self.assertEqual(shared['pool_generation'], 0xfedcba9876543210)
        self.assertEqual(client.delivery_info(delivery)['consumer_phase'], 'pending')
        exported = client.start_export(image, layer=1)
        export = exported['export_id']
        self.assertEqual(exported['total_bytes'], 48)
        self.assertEqual(client.export_info(export)['phase'], 'pending')
        with self.assertRaises(ControlError) as pending:
            client.read_export(export, length=10)
        self.assertEqual(pending.exception.code, ErrorCode.BUSY)
        self.destroy.touch()
        deadline = time.monotonic() + 3
        while client.sessions():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)
        self.assertTrue(client.release_image(image)['released'])
        self.assertEqual(client.delivery_info(delivery)['capture_id'], self.capture)
        self.ready.touch()
        while client.export_info(export)['phase'] == 'pending':
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)
        data = bytearray()
        while len(data) < 48:
            part = client.read_export(export, offset=len(data), length=13)
            self.assertEqual(part['offset'], len(data))
            data.extend(part['bytes'])
        self.assertTrue(part['end'])
        self.assertEqual(data, expected[48:])
        self.assertTrue(client.release_delivery(delivery)['released'])
        self.assertEqual(client.export_info(export)['phase'], 'complete')
        client.release_export(export)
        client.close()
        self.assertEqual(os.pread(handles[0], 96, 0), expected)  # Received handles outlive connection.
        descriptors = tuple(handles)
        handles.close()
        for fd in descriptors:
            with self.assertRaises(OSError):
                os.fstat(fd)


    def test_fragmented_rights_in_payload_replay_and_bad_ancillary_close(self):
        # Exercise LocalControlServer's recvmsg on both header and payload; all
        # exact retry descriptors are received again but identify one consumer.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.connect(str(self.path))
            connection.settimeout(2)
            def send(message):
                connection.sendall(message.encode())
                response, handles = packet_read(connection)
                self.assertEqual(handles, [])
                return to_dict(response.fields)
            send(Message(OpCode.HELLO, 1))
            records = send(Message(OpCode.SESSIONS, 2))['sessions']
            session = records[0]
            retained = send(Message(OpCode.RETAIN_IMAGE, 3, (Field.u64(Tag.SESSION, session['session']),
                Field.u64(Tag.GENERATION, session['generation']), Field.u64(Tag.CAPTURE_ID, self.capture))))
            description = retained['image_descriptor']
            message = Message(OpCode.SHARE_IMAGE, 4, (Field.u64(Tag.IMAGE_READER_ID, retained['image_reader_id']),
                Field.binary(Tag.DEVICE_UUID, description['device_uuid']), Field.binary(Tag.DRIVER_UUID, description['driver_uuid']),
                Field.u32(Tag.HANDLE_COUNT, 1)))
            encoded = message.encode()
            connection.sendall(encoded[:HEADER.size + 1])
            fd = os.open('/dev/null', os.O_RDONLY | os.O_CLOEXEC)
            try:
                self.assertEqual(connection.sendmsg([encoded[HEADER.size + 1:HEADER.size + 2]],
                    [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))]), 1)
                connection.sendall(encoded[HEADER.size + 2:])
                response, handles = packet_read(connection)
                self.assertEqual(len(handles), 2)
                original = to_dict(response.fields)
                connection.sendall(encoded)  # Same envelope, zero incoming descriptors.
                replay, duplicates = packet_read(connection)
                self.assertEqual(to_dict(replay.fields), original)
                self.assertEqual(len(duplicates), 2)
                for received in handles + duplicates:
                    self.assertFalse(os.get_inheritable(received))
                    os.close(received)
                connection.sendmsg([encoded], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
                rejected, rejected_handles = packet_read(connection)
                self.assertEqual(to_dict(rejected.fields)['error'], ErrorCode.CONFLICT)
                self.assertEqual(rejected_handles, [])
            finally:
                os.close(fd)
        # More than four rights is a transport violation; it closes the peer and
        # its own installed copies rather than silently dropping ancillary data.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.connect(str(self.path))
            connection.settimeout(2)
            fd = os.open('/dev/null', os.O_RDONLY | os.O_CLOEXEC)
            try:
                connection.sendmsg([Message(OpCode.HELLO, 1).encode()],
                    [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd] * 8))])
                self.assertEqual(connection.recv(1), b'')
            finally:
                os.close(fd)


if __name__ == '__main__':
    if len(sys.argv) > 1 and not sys.argv[1].startswith('-'):
        HOST = sys.argv.pop(1)
        # Decorators are evaluated before command-line handling.
        ImageEndpointTests.__unittest_skip__ = False
    unittest.main()
