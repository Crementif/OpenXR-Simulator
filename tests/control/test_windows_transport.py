"""CPU models of Windows overlapped ownership; these do not execute Win32."""
import ctypes
from pathlib import Path
import socket
import sys
import threading
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import windows_transport as transport
from simxr_control import client as sdk
from simxr_control import connection as packets
from simxr_control.protocol import Field, HEADER, Message, OpCode, ProtocolError, Tag


class Kernel:
    def __init__(self):
        self.error = 997
        self.delay = True
        self.incomplete = False
        self.started = 0
        self.closed = []
        self.operation = None
        self.buffer = None
        self.goal = 0
        self.write = False

    def CreateEventW(self, *_):
        return 7

    def ReadFile(self, _, buffer, goal, count, operation):
        self.started += 1
        self.operation, self.buffer, self.goal = operation._obj, buffer, goal
        return False

    def WriteFile(self, *arguments):
        self.write = True
        return self.ReadFile(*arguments)

    def WaitForSingleObject(self, *_):
        if self.delay:
            time.sleep(0.003)
            return 258
        return 0

    def GetOverlappedResult(self, _, operation, count, __):
        if self.incomplete:
            self.error = 996
            return False
        count._obj.value = self.goal if self.write else min(self.goal, 3)
        if not self.write:
            ctypes.memmove(self.buffer, b'abc', count._obj.value)
        return True

    def CloseHandle(self, handle):
        self.closed.append(handle)
        return True

    def CancelIoEx(self, *_):
        return True


class WindowsOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.kernel = Kernel()
        self.previous = transport._CHANNELS, transport._OWNERS
        transport._CHANNELS, transport._OWNERS = 0, [None] * 128
        self.api = patch.object(transport, '_api', return_value=(self.kernel, object()))
        self.error = patch.object(ctypes, 'get_last_error', side_effect=lambda: self.kernel.error, create=True)
        self.api.start()
        self.error.start()
        self.client = transport.NamedPipeSocket()
        self.client._handle = 42
        self.client._lease[0] = 42
        self.client.settimeout(0.001)

    def tearDown(self):
        self.kernel.delay = False
        self.client.close()
        self.api.stop()
        self.error.stop()
        transport._CHANNELS, transport._OWNERS = self.previous

    def test_partial_read_retry_keeps_one_native_operation(self):
        with self.assertRaises(socket.timeout):
            self.client.recv(32)
        native = self.client._pending
        self.assertIsNotNone(native)
        self.assertEqual(self.kernel.started, 1)
        self.kernel.delay = False
        self.client.settimeout(1)
        self.assertEqual(self.client.recv(32), b'abc')
        self.assertEqual(self.kernel.started, 1)
        self.assertIsNone(self.client._pending)
        self.assertEqual(self.kernel.closed, [7])

    def test_write_retry_never_reissues_accepted_request(self):
        with self.assertRaises(socket.timeout):
            self.client.send(b'identified request')
        with self.assertRaises(RuntimeError):
            self.client.send(b'a different packet')
        self.assertEqual(self.kernel.started, 1)
        self.kernel.delay = False
        self.client.settimeout(1)
        self.assertEqual(self.client.send(b'identified request'), len(b'identified request'))
        self.assertEqual(self.kernel.started, 1)

    def test_unfinished_cancel_preserves_preallocated_native_owners(self):
        with self.assertRaises(socket.timeout):
            self.client.recv(32)
        lease, pending, slot = self.client._lease, self.client._pending, self.client._slot
        self.kernel.incomplete, self.kernel.delay = True, False
        self.client.close()
        self.assertIs(transport._OWNERS[slot], lease)
        self.assertIs(lease[1], pending)
        self.assertEqual(lease[0], 42)
        self.assertEqual(transport._CHANNELS, 1)
        self.assertEqual(self.kernel.closed, [])

    def test_pre_admission_allocation_failure_closes_event(self):
        with patch.object(ctypes, 'create_string_buffer', side_effect=MemoryError):
            with self.assertRaises(MemoryError):
                self.client.recv(32)
        self.assertEqual(self.kernel.started, 0)
        self.assertIsNone(self.client._pending)
        self.assertEqual(self.kernel.closed, [7])

    def test_local_only_path_policy(self):
        transport._validate(r'\\.\pipe\SimXR.Control.1234')
        for path in (r'\\remote\pipe\SimXR.Control.1234', r'\\.\pipe\other.1234', r'\\.\pipe\SimXR.Control.x\suffix'):
            with self.assertRaises(ValueError):
                transport._validate(path)


class WindowsDiscoveryTests(unittest.TestCase):
    def test_enumeration_distinguishes_end_from_native_failure(self):
        for error in (18, 5):
            with self.subTest(error=error):
                kernel = Mock()

                def first(_, data):
                    data._obj.name = 'SimXR.Control.model'
                    return 9

                kernel.FindFirstFileW.side_effect = first
                kernel.FindNextFileW.return_value = False
                with patch.object(transport, '_api', return_value=(kernel, object())), \
                        patch.object(ctypes, 'get_last_error', return_value=error, create=True), \
                        patch.object(ctypes, 'WinError', side_effect=lambda code: OSError(code, 'native failure'), create=True):
                    if error == 18:
                        self.assertEqual(transport.discover(), [r'\\.\pipe\SimXR.Control.model'])
                    else:
                        with self.assertRaises(OSError) as raised:
                            transport.discover()
                        self.assertEqual(raised.exception.errno, error)
                kernel.FindClose.assert_called_once_with(9)


class FramedPipe:
    def __init__(self):
        self.sent, self.reply = bytearray(), bytearray()
        self.opcodes = []
        self.closed = False

    def settimeout(self, _):
        pass

    def connect(self, path):
        transport._validate(path)

    def send(self, data):
        count = min(7, len(data))
        self.sent.extend(data[:count])
        if len(self.sent) >= HEADER.size and len(self.sent) == Message.size(self.sent[:HEADER.size]):
            request = Message.decode(self.sent)
            self.opcodes.append(request.opcode)
            self.reply.extend(Message(request.opcode, request.request_id,
                                      (Field.u32(Tag.ERROR, 0), Field.u64(Tag.CAPABILITIES, 1)), flags=1).encode())
            self.sent.clear()
        return count

    def recvmsg(self, count):
        count = min(5, count, len(self.reply))
        result = bytes(self.reply[:count])
        del self.reply[:count]
        return result, [], 0, None

    def shutdown(self, *_):
        self.closed = True

    def close(self):
        self.closed = True


class WindowsSdkWireTests(unittest.TestCase):
    def test_packet_close_interrupts_pending_pipe_read(self):
        class BlockedPipe(FramedPipe):
            def __init__(self):
                super().__init__()
                self.waiting = threading.Event()
                self.stopping = threading.Event()

            def recvmsg(self, count):
                if self.opcodes[-1] == OpCode.SESSIONS:
                    self.waiting.set()
                    self.stopping.wait(3)
                    raise ConnectionError('Control pipe is closing')
                return super().recvmsg(count)

            def shutdown(self, *_):
                self.stopping.set()
                super().shutdown()

        pipe = BlockedPipe()
        errors, closed = [], threading.Event()
        with patch.object(packets.os, 'name', 'nt'), patch.object(transport, 'NamedPipeSocket', return_value=pipe):
            client = sdk.Client(r'\\.\pipe\SimXR.Control.model', timeout=5)

            def exchange():
                try:
                    client.request(OpCode.SESSIONS)
                except BaseException as error:
                    errors.append(error)

            def close():
                try:
                    client.close()
                finally:
                    closed.set()

            request = threading.Thread(target=exchange)
            closer = threading.Thread(target=close)
            try:
                request.start()
                self.assertTrue(pipe.waiting.wait(1))
                closer.start()
                self.assertTrue(closed.wait(1), 'Packet lock prevented pipe cancellation')
            finally:
                pipe.stopping.set()
                request.join(4)
                if closer.ident is not None:
                    closer.join(4)
                client.close()
            self.assertFalse(request.is_alive())
            self.assertEqual(len(errors), 1)
            self.assertIsInstance(errors[0], ConnectionError)
            self.assertTrue(pipe.closed)

    def test_client_uses_pipe_framing_without_unix_stat_or_fd_duplication(self):
        pipe = FramedPipe()
        with patch.object(packets.os, 'name', 'nt'), patch.object(transport, 'NamedPipeSocket', return_value=pipe), \
                patch.object(packets.os, 'stat', side_effect=AssertionError('Windows pipe must not be stat-ed as Unix socket')):
            with sdk.Client(r'\\.\pipe\SimXR.Control.model') as client:
                self.assertEqual(client.request(OpCode.SESSIONS)['error'], 0)
                with patch.object(sdk.ReceivedHandles, 'duplicate', side_effect=AssertionError('Do not duplicate unsupported handles')):
                    with self.assertRaises(ProtocolError):
                        client.request(OpCode.SHARE_IMAGE, handles=(99,))
            self.assertTrue(pipe.closed)
        self.assertEqual(pipe.opcodes, [OpCode.HELLO, OpCode.SESSIONS])


if __name__ == '__main__':
    unittest.main()
