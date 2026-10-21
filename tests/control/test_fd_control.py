"""Real Unix ancillary transport tests; pipes are CPU fixtures, not GPU fences."""
import array
from contextlib import ExitStack, contextmanager
import gc
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Client, ControlError, ControlTimeout, ProtocolError, ReceivedHandles
from simxr_control.protocol import Field, HEADER, Message, OpCode, Tag, to_dict


def packet_read(connection):
    data, handles, total = bytearray(), [], HEADER.size
    try:
        while len(data) < total:
            chunk, ancillary, flags, _ = connection.recvmsg(total-len(data), socket.CMSG_SPACE(16), socket.MSG_CMSG_CLOEXEC)
            if not chunk:
                raise EOFError('Peer closed unfinished test packet')
            if flags & ~socket.MSG_CMSG_CLOEXEC:
                raise AssertionError('Unexpected test ancillary flags')
            for level, kind, payload in ancillary:
                if level != socket.SOL_SOCKET or kind != socket.SCM_RIGHTS:
                    raise AssertionError('Unexpected test ancillary type')
                values = array.array('i')
                values.frombytes(payload)
                handles.extend(values)
            data.extend(chunk)
            if len(data) == HEADER.size:
                total = Message.size(data)
        return Message.decode(data), handles
    except BaseException:
        for fd in handles:
            os.close(fd)
        raise


def send_rights(connection, data, handles):
    return connection.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', handles))])


def response(request, *fields):
    return Message(request.opcode, request.request_id, (Field.u32(Tag.ERROR, 0, True),)+fields, flags=1).encode()


@contextmanager
def server(callback):
    with tempfile.TemporaryDirectory(prefix='simxr-fds-') as temporary:
        path = str(Path(temporary) / 'control.sock')
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(path)
        os.chmod(path, 0o600)
        listener.listen(1)
        errors = []

        def serve():
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(2)
                    hello, handles = packet_read(connection)
                    if handles:
                        for fd in handles:
                            os.close(fd)
                        raise AssertionError('Hello transferred unexpected FDs')
                    connection.sendall(response(hello))
                    callback(connection)
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            yield path
        finally:
            listener.close()
            thread.join(3)
            if thread.is_alive():
                raise AssertionError('FD fixture did not finish')
            if errors:
                raise errors[0]


class PartialSend:
    def __init__(self, native, before_positive=False):
        self.native = native
        self.before_positive = before_positive
        self.calls = 0
        self.fail_plain = True

    def __getattr__(self, name):
        return getattr(self.native, name)

    def sendmsg(self, buffers, ancillary):
        self.calls += 1
        if self.before_positive and self.calls == 1:
            raise socket.timeout()
        return self.native.sendmsg([buffers[0][:1]], ancillary)

    def send(self, data):
        if self.fail_plain:
            self.fail_plain = False
            raise socket.timeout()
        return self.native.send(data)


class FdTests(unittest.TestCase):
    def test_owned_wrapper_context_detach_and_gc(self):
        read, write = os.pipe()
        try:
            duplicate = ReceivedHandles.duplicate([read])
            fd = duplicate[0]
            self.assertNotEqual(fd, read)
            self.assertFalse(os.get_inheritable(fd))
            with duplicate:
                os.fstat(read)
            with self.assertRaises(OSError):
                os.fstat(fd)
            duplicate = ReceivedHandles.duplicate([read])
            detached, = duplicate.detach()
            duplicate.close()
            os.fstat(detached)
            os.close(detached)
            temporary = ReceivedHandles.duplicate([read])
            fd = temporary[0]
            del temporary
            gc.collect()
            with self.assertRaises(OSError):
                os.fstat(fd)
            with self.assertRaises((OSError, ProtocolError)):
                ReceivedHandles.duplicate([read, -1])
            os.fstat(read)
        finally:
            os.close(read)
            os.close(write)

    def test_structured_error_retires_only_exchange_and_next_request_is_fresh(self):
        identities = []
        def serve(connection):
            request, handles = packet_read(connection)
            self.assertEqual(handles, [])
            identities.append(request.request_id)
            connection.sendall(Message(request.opcode, request.request_id,
                (Field.u32(Tag.ERROR, 6, True),), flags=1).encode())
            request, handles = packet_read(connection)
            self.assertEqual(handles, [])
            identities.append(request.request_id)
            connection.sendall(response(request))
        with server(serve) as path, Client(path) as client:
            client.timeout = .5
            self.assertEqual(client.timeout, .5)
            with self.assertRaises(ControlError) as failure:
                client.request(OpCode.STATUS)
            self.assertEqual(failure.exception.code.name, 'NOT_FOUND')
            self.assertIsNone(client._connection._pending)
            self.assertEqual(client.request(OpCode.SESSIONS)['error'], 0)
        self.assertEqual(identities, [2, 3])

    def test_allocation_failure_before_publication_preserves_next_fd_exchange(self):
        import unittest.mock
        import simxr_control.connection as connection_module
        read, write = os.pipe()
        try:
            def serve(connection):
                request, incoming = packet_read(connection)
                try:
                    self.assertEqual(len(incoming), 1)
                    encoded = response(request, Field.u32(Tag.HANDLE_COUNT, 2))
                    send_rights(connection, encoded, [incoming[0], incoming[0]])
                finally:
                    for fd in incoming:
                        os.close(fd)
            with server(serve) as path, Client(path) as client:
                before = len(os.listdir('/proc/self/fd'))
                factory = unittest.mock.Mock(side_effect=MemoryError('incoming wrapper allocation'))
                factory.duplicate = ReceivedHandles.duplicate
                with unittest.mock.patch.object(connection_module, 'ReceivedHandles', factory):
                    with self.assertRaises(MemoryError):
                        client.share_image(1, bytes(16), bytes(16), read)
                self.assertIsNone(client._connection._pending)
                self.assertEqual(client._connection._next_request, 2)
                self.assertEqual(len(os.listdir('/proc/self/fd')), before)
                os.fstat(read)
                result = client.share_image(1, bytes(16), bytes(16), read)
                with result['_handles'] as handles:
                    self.assertEqual(len(handles), 2)
        finally:
            os.close(read)
            os.close(write)

    def test_receive_fragmented_rights_survive_timeout_and_connection(self):
        with ExitStack() as resources:
            pipes = [os.pipe() for _ in range(3)]
            for read, write in pipes:
                resources.callback(os.close, read)
                resources.callback(os.close, write)
            requests = []

            def serve(connection):
                request, incoming = packet_read(connection)
                try:
                    self.assertEqual(len(incoming), 1)
                    self.assertFalse(os.get_inheritable(incoming[0]))
                    self.assertEqual(to_dict(request.fields)['handle_count'], 1)
                    requests.append(request.request_id)
                    encoded = response(request, Field.u32(Tag.HANDLE_COUNT, 2), Field.u64(Tag.DELIVERY_ID, 2**64-1),
                                       Field.binary(Tag.DEVICE_UUID, bytes(range(16))),
                                       Field.nested(Tag.IMAGE_DESCRIPTOR, [Field.u32(Tag.WIDTH, 4), Field.boolean(Tag.DEDICATED, True)]))
                    send_rights(connection, encoded[:12], [pipes[1][0]])
                    connection.sendall(encoded[12:HEADER.size])
                    time.sleep(0.12)
                    send_rights(connection, encoded[HEADER.size:HEADER.size+1], [pipes[2][0]])
                    connection.sendall(encoded[HEADER.size+1:])
                    self.assertEqual(connection.recv(1), b'')
                finally:
                    for fd in incoming:
                        os.close(fd)

            with server(serve) as path:
                client = Client(path, timeout=0.04)
                with self.assertRaises(ControlTimeout):
                    client.share_image(77, bytes(16), bytes(16), pipes[0][0])
                self.assertEqual(len(client._connection._pending._incoming_handles), 1)
                os.fstat(pipes[0][0])  # Caller FD remains independently owned.
                time.sleep(0.14)
                record = client.retry()
                self.assertEqual(record['delivery_id'], 2**64-1)
                self.assertEqual(record['device_uuid'], bytes(range(16)))
                self.assertEqual(record['image_descriptor'], {'width': 4, 'dedicated': True})
                handles = record['_handles']
                self.assertEqual(len(handles), 2)
                self.assertTrue(all(not os.get_inheritable(fd) for fd in handles))
                client.close()
                saved = tuple(handles)
                for fd in saved:
                    os.fstat(fd)  # Returned handles outlive SDK connection.
                handles.close()
                handles.close()
                for fd in saved:
                    with self.assertRaises(OSError):
                        os.fstat(fd)
            self.assertEqual(len(requests), 1)

    def test_partial_positive_send_never_repeats_rights(self):
        self._partial_send(False)

    def test_timeout_before_positive_send_keeps_duplicated_rights(self):
        self._partial_send(True)

    def _partial_send(self, before_positive):
        read, write = os.pipe()
        try:
            observed = []

            def serve(connection):
                request, incoming = packet_read(connection)
                try:
                    self.assertEqual(len(incoming), 1)
                    observed.append(incoming[0])
                    os.fstat(incoming[0])
                    connection.sendall(response(request, Field.text(Tag.DETAIL, 'one exchange')))
                finally:
                    for fd in incoming:
                        os.close(fd)

            with server(serve) as path:
                with Client(path, timeout=0.1) as client:
                    wrapped = PartialSend(client._connection._socket, before_positive)
                    client._connection._socket = wrapped
                    with self.assertRaises(ControlTimeout):
                        client.request(OpCode.STATUS, handles=(read,))
                    os.fstat(read)
                    os.close(read)
                    read = -1
                    if before_positive:
                        self.assertEqual(len(client._connection._pending._outgoing_handles), 1)
                        with self.assertRaises(ControlTimeout):
                            client.retry()  # First actual positive byte, then a plain-send timeout.
                    self.assertEqual(client.retry()['detail'], 'one exchange')
                    self.assertEqual(wrapped.calls, 2 if before_positive else 1)
            self.assertEqual(len(observed), 1)
        finally:
            if read >= 0:
                os.close(read)
            os.close(write)

    def test_close_releases_partial_incoming_rights(self):
        read, write = os.pipe()
        done = threading.Event()
        try:
            def serve(connection):
                request, incoming = packet_read(connection)
                self.assertEqual(incoming, [])
                encoded = response(request, Field.u32(Tag.HANDLE_COUNT, 2))
                send_rights(connection, encoded[:5], [read])
                self.assertTrue(done.wait(2))

            with server(serve) as path:
                client = Client(path, timeout=0.03)
                try:
                    with self.assertRaises(ControlTimeout):
                        client.request(OpCode.SHARE_IMAGE)
                    received, = tuple(client._connection._pending._incoming_handles)
                    client.close()
                    with self.assertRaises(OSError):
                        os.fstat(received)
                finally:
                    client.close()
                    done.set()
        finally:
            os.close(read)
            os.close(write)

    def test_malformed_ancillary_closes_every_received_right(self):
        for mode in ('truncated', 'accumulated', 'count', 'error'):
            with self.subTest(mode=mode):
                self._malformed(mode)

    def _malformed(self, mode):
        baseline = len(os.listdir('/proc/self/fd'))
        with ExitStack() as resources:
            pipes = [os.pipe() for _ in range(8)]
            for read, write in pipes:
                resources.callback(os.close, read)
                resources.callback(os.close, write)

            def serve(connection):
                request, incoming = packet_read(connection)
                self.assertEqual(incoming, [])
                fields = [Field.u32(Tag.ERROR, 5 if mode == 'error' else 0, True),
                          Field.u32(Tag.HANDLE_COUNT, 1 if mode == 'error' else 2)]
                encoded = Message(request.opcode, request.request_id, tuple(fields), flags=1).encode()
                try:
                    if mode == 'truncated':
                        send_rights(connection, encoded, [pair[0] for pair in pipes])
                    elif mode == 'accumulated':
                        send_rights(connection, encoded[:HEADER.size], [pair[0] for pair in pipes[:4]])
                        send_rights(connection, encoded[HEADER.size:], [pipes[4][0]])
                    else:
                        send_rights(connection, encoded, [pipes[0][0]])
                except (BrokenPipeError, ConnectionResetError):
                    pass

            with server(serve) as path:
                with Client(path) as client:
                    with self.assertRaises(ProtocolError):
                        client.request(OpCode.SHARE_IMAGE)
                    self.assertIsNone(client._connection._pending)
                    self.assertTrue(client._connection._closed)
        gc.collect()
        self.assertEqual(len(os.listdir('/proc/self/fd')), baseline)

    def test_unexpected_control_type_still_closes_rights(self):
        read, write = os.pipe()
        duplicate = os.dup(read)
        try:
            class FakeSocket:
                def recvmsg(self, *_):
                    return b'x', [(999, 999, b'bad'), (socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [duplicate]).tobytes())], 0, None
            from simxr_control.connection import PendingExchange
            exchange = PendingExchange(OpCode.STATUS, 1, (), ())
            with self.assertRaises(ProtocolError):
                exchange._receive(FakeSocket(), 1)
            exchange.close()
            with self.assertRaises(OSError):
                os.fstat(duplicate)
        finally:
            os.close(read)
            os.close(write)

    def test_gpu_schema_and_bounded_export_forwarding(self):
        client = object.__new__(Client)
        requests = []
        client.request = lambda opcode, fields=(), **kwargs: requests.append((opcode, to_dict(fields), kwargs))
        client.start_export(2**64-1, layer=1)
        client.read_export(2**64-1, offset=2**63, length=256)
        self.assertEqual(requests[0][1], {'image_reader_id': 2**64-1, 'layer': 1})
        self.assertEqual(requests[1][1], {'export_id': 2**64-1, 'offset': 2**63, 'length': 256})
        with self.assertRaises(ProtocolError):
            client.read_export(1, length=1024*1024)
        with self.assertRaises(ProtocolError):
            client.share_image(1, bytes(15), bytes(16), 0)
        client.share_image(1, bytes(16), bytes(range(16)), 42)
        self.assertEqual(requests[-1][2], {'handles': (42,)})
        with self.assertRaises(ProtocolError):
            to_dict([Field.binary(Tag.DRIVER_UUID, b'short')])


if __name__ == '__main__':
    unittest.main()
