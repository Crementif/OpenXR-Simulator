"""Serialized packet transport with one explicitly retryable unfinished exchange.

PendingExchange owns partial framing and native handles. PacketConnection owns
socket lifetime and request publication; no typed simulator state lives here.
"""
import array
import os
import socket
import stat
import threading
import time

from .errors import ControlError, ControlTimeout
from .protocol import (Field, HEADER, MAX_TRANSFER_HANDLES, Message, OpCode,
                       ProtocolError, Tag, to_dict)


class ReceivedHandles:
    """Owned native FDs. Close explicitly or use a context manager.

    Iteration/indexing borrows the descriptor numbers. detach() transfers all
    ownership to the caller, for APIs that consume imported native handles.
    A discarded wrapper closes its remaining descriptors automatically.
    """
    def __init__(self):
        self._fds = []
        self._lock = threading.Lock()

    def __len__(self):
        with self._lock:
            return len(self._fds)

    def __iter__(self):
        with self._lock:
            return iter(tuple(self._fds))

    def __getitem__(self, index):
        with self._lock:
            return self._fds[index]

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        try:
            self.close()
        except BaseException:
            pass

    def _append(self, fd):
        with self._lock:
            self._fds.append(fd)

    def detach(self):
        with self._lock:
            result = tuple(self._fds)
            self._fds.clear()
            return result

    def close(self):
        # Cleanup must not allocate a tuple: it also runs after failed decoding
        # or ownership allocation and from __del__.
        while True:
            with self._lock:
                if not self._fds:
                    return
                fd = self._fds.pop()
            try:
                os.close(fd)
            except OSError:
                pass

    @classmethod
    def duplicate(cls, descriptors):
        result = cls()
        try:
            for descriptor in descriptors:
                if len(result) == MAX_TRANSFER_HANDLES:
                    raise ProtocolError('Too many outgoing native handles')
                if type(descriptor) is not int or descriptor < 0:
                    raise ProtocolError('A valid native FD integer is required')
                duplicate = os.dup(descriptor)
                try:
                    os.set_inheritable(duplicate, False)
                    result._append(duplicate)
                except BaseException:
                    os.close(duplicate)
                    raise
            return result
        except BaseException:
            result.close()
            raise

class PendingExchange:
    def __init__(self, opcode, request_id, fields, handles):
        self._outgoing_handles = ReceivedHandles.duplicate(handles)
        try:
            fields = tuple(fields)
            count = next((field for field in fields if field.tag == Tag.HANDLE_COUNT), None)
            if count is not None and count.as_u32() != len(self._outgoing_handles):
                raise ProtocolError('HandleCount does not match outgoing native handles')
            if count is None and len(self._outgoing_handles):
                fields += (Field.u32(Tag.HANDLE_COUNT, len(self._outgoing_handles), True),)
            self.request = Message(opcode, request_id, fields)
            self.packet = self.request.encode()
            self._incoming_handles = ReceivedHandles()
            self._received = bytearray()
            self._expected = HEADER.size
            self._sent = 0
            self._handles_sent = False
        except BaseException:
            self._outgoing_handles.close()
            raise

    def close(self):
        self._outgoing_handles.close()
        self._incoming_handles.close()

    def complete(self, transport, timeout):
        request, packet = self.request, self.packet
        deadline = time.monotonic() + timeout
        while self._sent < len(packet):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout()
            transport.settimeout(remaining)
            if not self._handles_sent and len(self._outgoing_handles):
                rights = array.array('i', tuple(self._outgoing_handles))
                count = transport.sendmsg([packet[self._sent:]], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, rights)])
            else:
                count = transport.send(packet[self._sent:])
            if not count:
                raise ConnectionError('Control peer closed while sending')
            if not self._handles_sent:
                self._handles_sent = True
                self._outgoing_handles.close()
            self._sent += count
        while True:
            while len(self._received) < self._expected:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise socket.timeout()
                transport.settimeout(remaining)
                chunk = self._receive(transport, self._expected - len(self._received))
                if not chunk:
                    raise ConnectionError('Control peer closed while receiving')
                self._received.extend(chunk)
            if self._expected == HEADER.size:
                total = Message.size(self._received)
                if total > HEADER.size:
                    self._expected = total
                    continue
            response = Message.decode(self._received)
            if response.flags != 1 or response.request_id != request.request_id or response.opcode != request.opcode:
                raise ProtocolError('Response does not match request')
            record = to_dict(response.fields)
            if 'error' not in record:
                raise ProtocolError('Response omitted its error status')
            if record.get('handle_count', 0) != len(self._incoming_handles):
                raise ProtocolError('Response HandleCount does not match received rights')
            if len(self._incoming_handles) and request.opcode != OpCode.SHARE_IMAGE:
                raise ProtocolError('Unexpected native handles on this response opcode')
            if not record['error'] and request.opcode == OpCode.SHARE_IMAGE and len(self._incoming_handles) != 2:
                raise ProtocolError('Successful image sharing must transfer memory and readiness handles')
            if record['error']:
                if len(self._incoming_handles):
                    raise ProtocolError('Error response unexpectedly transferred native handles')
                raise ControlError(record)
            if len(self._incoming_handles):
                record['_handles'] = self._incoming_handles
                self._incoming_handles = ReceivedHandles()
            return record

    def _receive(self, transport, count):
        if os.name == 'nt':
            chunk, ancillary, flags, _ = transport.recvmsg(count)
            if ancillary or flags:
                raise ProtocolError('Windows control response contains unsupported native handles')
            return chunk
        size = array.array('i').itemsize
        descriptors = []
        adopted = 0
        chunk, ancillary, flags, _ = transport.recvmsg(
            count, socket.CMSG_SPACE(MAX_TRANSFER_HANDLES * size), getattr(socket, 'MSG_CMSG_CLOEXEC', 0))
        malformed = bool(flags & ~getattr(socket, 'MSG_CMSG_CLOEXEC', 0))
        try:
            for level, kind, payload in ancillary:
                if level != socket.SOL_SOCKET or kind != socket.SCM_RIGHTS:
                    malformed = True
                    continue
                rights = array.array('i')
                rights.frombytes(payload[:len(payload) - len(payload) % size])
                descriptors.extend(rights)
                malformed |= bool(len(payload) % size)
            if malformed or len(self._incoming_handles) + len(descriptors) > MAX_TRANSFER_HANDLES:
                raise ProtocolError('Malformed, truncated, or excessive ancillary handles')
            for fd in descriptors:
                if fd < 0:
                    raise ProtocolError('Invalid received native descriptor')
                os.set_inheritable(fd, False)
                self._incoming_handles._append(fd)
                adopted += 1
            return chunk
        finally:
            # The original ancillary bytes still own every unadopted FD, even
            # if constructing the decoded descriptor list failed partway.
            for level, kind, payload in ancillary:
                if level != socket.SOL_SOCKET or kind != socket.SCM_RIGHTS:
                    continue
                values = memoryview(payload)[:len(payload) - len(payload) % size].cast('i')
                for fd in values:
                    if adopted:
                        adopted -= 1
                    elif fd >= 0:
                        try:
                            os.close(fd)
                        except OSError:
                            pass



class PacketConnection:
    def __init__(self, path, timeout):
        if not timeout > 0:
            raise ValueError('A positive control timeout is required')
        self.path = str(path)
        self.timeout = timeout
        self._lock = threading.Lock()
        self._next_request = 1
        self._pending = None
        self._closed = False
        if os.name == 'nt':
            from .windows_transport import NamedPipeSocket
            self._socket = NamedPipeSocket()
        else:
            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self._socket.settimeout(timeout)
            if os.name != 'nt':
                info = os.stat(self.path)
                if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
                    raise PermissionError('Control endpoint must be a private socket owned by this user')
            self._socket.connect(self.path)
        except BaseException:
            self._socket.close()
            self._closed = True
            raise

    def close(self):
        with self._lock:
            self._close()

    def _close(self):
        if not self._closed:
            self._closed = True
            try:
                self._socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._socket.close()
        if self._pending is not None:
            self._pending.close()
            self._pending = None

    def request(self, opcode, fields=(), *, handles=()):
        with self._lock:
            if self._closed:
                raise ConnectionError('Control connection closed')
            if self._pending is not None:
                raise ControlTimeout(self._pending.request.request_id)
            if os.name == 'nt' and handles:
                raise ProtocolError('Native GPU handle transfer is unavailable over Windows named pipes')
            # Nothing is published until encoding and all ownership allocation
            # succeeds. Failure leaves both identity and previous transport intact.
            pending = PendingExchange(opcode, self._next_request, fields, handles)
            try:
                next_request = self._next_request + 1
            except BaseException:
                pending.close()
                raise
            self._pending = pending
            self._next_request = next_request
            return self._complete()

    def retry(self):
        with self._lock:
            if self._closed or self._pending is None:
                raise ValueError('No unfinished exchange')
            return self._complete()

    def _complete(self):
        pending = self._pending
        try:
            result = pending.complete(self._socket, self.timeout)
        except socket.timeout as error:
            # The same object retains every byte offset and received FD. No
            # later request is admitted until retry finishes or close retires it.
            raise ControlTimeout(pending.request.request_id) from error
        except ControlError:
            self._pending = None
            pending.close()
            raise
        except BaseException:
            self._close()
            raise
        self._pending = None
        pending.close()
        return result
