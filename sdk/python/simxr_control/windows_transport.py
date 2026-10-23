"""Local, same-user Windows named pipes using only ctypes and the standard library.

The socket-shaped adapter retains an overlapped operation across socket.timeout;
retrying send/recv resumes that operation rather than writing another request.
Native GPU handle transfer is deliberately unsupported.
"""
import ctypes
import math
import os
import socket
import threading
import time

_PREFIX = '\\\\.\\pipe\\SimXR.Control.'
_INVALID = ctypes.c_void_p(-1).value
_PENDING = 997
_INCOMPLETE = 996
_API = None
_BUDGET_LOCK = threading.Lock()
_CHANNELS = 0
_OWNERS = [None] * 128


class _Overlapped(ctypes.Structure):
    _fields_ = [('internal', ctypes.c_size_t), ('internal_high', ctypes.c_size_t),
                ('offset', ctypes.c_uint32), ('offset_high', ctypes.c_uint32),
                ('event', ctypes.c_void_p)]


class _FileTime(ctypes.Structure):
    _fields_ = [('low', ctypes.c_uint32), ('high', ctypes.c_uint32)]


class _FindData(ctypes.Structure):
    _fields_ = [('attributes', ctypes.c_uint32), ('created', _FileTime),
                ('accessed', _FileTime), ('written', _FileTime),
                ('size_high', ctypes.c_uint32), ('size_low', ctypes.c_uint32),
                ('reserved0', ctypes.c_uint32), ('reserved1', ctypes.c_uint32),
                ('name', ctypes.c_wchar * 260), ('alternate', ctypes.c_wchar * 14)]


def _api():
    global _API
    if _API is not None:
        return _API
    if os.name != 'nt':
        raise OSError('Windows named pipes require native Windows Python')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    security = ctypes.WinDLL('advapi32', use_last_error=True)
    handle, dword, boolean = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int
    pointer = ctypes.POINTER
    prototypes = {
        'CreateFileW': (handle, [ctypes.c_wchar_p, dword, dword, handle, dword, dword, handle]),
        'CreateEventW': (handle, [handle, boolean, boolean, ctypes.c_wchar_p]),
        'CloseHandle': (boolean, [handle]), 'SetEvent': (boolean, [handle]),
        'ReadFile': (boolean, [handle, handle, dword, pointer(dword), pointer(_Overlapped)]),
        'WriteFile': (boolean, [handle, handle, dword, pointer(dword), pointer(_Overlapped)]),
        'GetOverlappedResult': (boolean, [handle, pointer(_Overlapped), pointer(dword), boolean]),
        'CancelIoEx': (boolean, [handle, pointer(_Overlapped)]),
        'WaitForSingleObject': (dword, [handle, dword]),
        'WaitNamedPipeW': (boolean, [ctypes.c_wchar_p, dword]),
        'GetNamedPipeServerProcessId': (boolean, [handle, pointer(dword)]),
        'GetCurrentProcess': (handle, []), 'OpenProcess': (handle, [dword, boolean, dword]),
        'FindFirstFileW': (handle, [ctypes.c_wchar_p, pointer(_FindData)]),
        'FindNextFileW': (boolean, [handle, pointer(_FindData)]),
        'FindClose': (boolean, [handle]),
    }
    for name, (result, arguments) in prototypes.items():
        function = getattr(kernel, name)
        function.restype, function.argtypes = result, arguments
    prototypes = {
        'OpenProcessToken': (boolean, [handle, dword, pointer(handle)]),
        'GetTokenInformation': (boolean, [handle, ctypes.c_int, handle, dword, pointer(dword)]),
        'GetLengthSid': (dword, [handle]), 'EqualSid': (boolean, [handle, handle]),
    }
    for name, (result, arguments) in prototypes.items():
        function = getattr(security, name)
        function.restype, function.argtypes = result, arguments
    _API = kernel, security
    return _API


def _validate(path):
    if not isinstance(path, str) or not path.startswith(_PREFIX) or not len(_PREFIX) < len(path) <= 240:
        raise ValueError('Expected a local \\\\.\\pipe\\SimXR.Control.NAME endpoint')
    if any(not (character.isascii() and (character.isalnum() or character in '._-')) for character in path[len(_PREFIX):]):
        raise ValueError('Invalid local control pipe name')


def discover():
    kernel, _ = _api()
    found = _FindData()
    search = kernel.FindFirstFileW(_PREFIX + '*', ctypes.byref(found))
    if search == _INVALID:
        error = ctypes.get_last_error()
        if error in (2, 3, 18):
            return []
        raise ctypes.WinError(error)
    result = []
    try:
        while True:
            candidate = '\\\\.\\pipe\\' + found.name
            try:
                _validate(candidate)
                result.append(candidate)
            except ValueError:
                pass
            if len(result) == 64:
                break
            if not kernel.FindNextFileW(search, ctypes.byref(found)):
                error = ctypes.get_last_error()
                if error != 18:  # ERROR_NO_MORE_FILES
                    raise ctypes.WinError(error)
                break
    finally:
        kernel.FindClose(search)
    return sorted(set(result))


def _sid(process):
    kernel, security = _api()
    token = ctypes.c_void_p()
    if not security.OpenProcessToken(process, 8, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        size = ctypes.c_uint32()
        security.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))
        if not size.value or size.value > 65536:
            raise PermissionError('Invalid Windows peer-token information')
        data = ctypes.create_string_buffer(size.value)
        if not security.GetTokenInformation(token, 1, data, len(data), ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        pointer = ctypes.cast(data, ctypes.POINTER(ctypes.c_void_p))[0]
        length = security.GetLengthSid(pointer)
        if not length or length > 1024:
            raise PermissionError('Invalid Windows peer SID')
        return ctypes.create_string_buffer(ctypes.string_at(pointer, length))
    finally:
        kernel.CloseHandle(token)


class NamedPipeSocket:
    """Socket-shaped byte stream. It never accepts SCM_RIGHTS or raw handles."""
    def __init__(self):
        global _CHANNELS
        self._kernel, self._security = _api()
        self._handle = None
        self._pending = None
        self._timeout = 2.0
        self._lock = threading.RLock()
        self._stopping = threading.Event()
        self._charged = False
        self._lease = [None, None]
        self._slot = None
        with _BUDGET_LOCK:
            if _CHANNELS >= 128:
                raise RuntimeError('Windows control native-owner capacity exhausted')
            self._slot = _OWNERS.index(None)
            _OWNERS[self._slot] = True
            _CHANNELS += 1
            self._charged = True

    def settimeout(self, seconds):
        if not isinstance(seconds, (float, int)) or not math.isfinite(seconds) or not 0 < seconds <= 86400:
            raise ValueError('A bounded positive pipe timeout is required')
        self._timeout = seconds

    def connect(self, path):
        if self._handle is not None or self._stopping.is_set():
            raise ConnectionError("Control pipe is already connected or closed")
        _validate(path)
        deadline = time.monotonic() + self._timeout
        while True:
            handle = self._kernel.CreateFileW(path, 0xC0000000, 0, None, 3, 0x40000000 | 0x100000 | 0x10000, None)
            if handle != _INVALID:
                self._handle = handle
                self._lease[0] = handle
                break
            error = ctypes.get_last_error()
            if error not in (2, 231):
                raise ctypes.WinError(error)
            if time.monotonic() >= deadline:
                raise socket.timeout('Connect to local control pipe timed out')
            if error == 231:
                self._kernel.WaitNamedPipeW(path, 10)
            else:
                time.sleep(0.005)
        process_id = ctypes.c_uint32()
        if not self._kernel.GetNamedPipeServerProcessId(self._handle, ctypes.byref(process_id)):
            raise ctypes.WinError(ctypes.get_last_error())
        process = self._kernel.OpenProcess(0x1000, False, process_id.value)
        if not process:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            peer, own = _sid(process), _sid(self._kernel.GetCurrentProcess())
            if not self._security.EqualSid(peer, own):
                raise PermissionError('Control pipe server belongs to another Windows user')
        finally:
            self._kernel.CloseHandle(process)

    def _transfer(self, kind, data):
        with self._lock:
            if not self._handle or self._stopping.is_set():
                raise ConnectionError('Control pipe is closed')
            goal = min(len(data) if kind == 'write' else data, 65536)
            if goal <= 0:
                return 0 if kind == 'write' else b''
            count = ctypes.c_uint32()
            if self._pending is None:
                event = self._kernel.CreateEventW(None, True, False, None)
                if not event:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    operation = _Overlapped(event=event)
                    payload = bytes(data[:goal]) if kind == 'write' else None
                    buffer = ctypes.create_string_buffer(payload, goal) if kind == 'write' else ctypes.create_string_buffer(goal)
                    pending = (kind, goal, buffer, operation, payload)
                except BaseException:
                    self._kernel.CloseHandle(event)
                    raise
                self._lease[1] = self._pending = pending
                complete = (self._kernel.WriteFile if kind == 'write' else self._kernel.ReadFile)(
                    self._handle, buffer, goal, ctypes.byref(count), ctypes.byref(operation))
                if complete:
                    self._lease[1] = self._pending = None
                    self._kernel.CloseHandle(event)
                    if not 0 < count.value <= goal:
                        raise ConnectionError('Control pipe peer disconnected')
                    return count.value if kind == 'write' else buffer.raw[:count.value]
                error = ctypes.get_last_error()
                if error != _PENDING:
                    self._lease[1] = self._pending = None
                    self._kernel.CloseHandle(event)
                    raise ctypes.WinError(error)
            pending = self._pending
            if pending[0] != kind or pending[1] != goal or (kind == 'write' and pending[4] != bytes(data[:goal])):
                raise RuntimeError('Retry must preserve the same unfinished pipe operation')
            _, _, buffer, operation, _ = pending
            deadline = time.monotonic() + self._timeout
            while True:
                if self._stopping.is_set():
                    raise ConnectionError('Control pipe is closing')
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise socket.timeout('Control pipe operation remains pending')
                waited = self._kernel.WaitForSingleObject(operation.event, min(50, max(1, math.ceil(remaining * 1000))))
                if waited == 258:
                    continue
                if waited != 0:
                    raise ctypes.WinError(ctypes.get_last_error())
                complete = self._kernel.GetOverlappedResult(self._handle, ctypes.byref(operation), ctypes.byref(count), False)
                error = 0 if complete else ctypes.get_last_error()
                if error == _INCOMPLETE:
                    continue
                self._lease[1] = self._pending = None
                self._kernel.CloseHandle(operation.event)
                if not complete:
                    raise ctypes.WinError(error)
                if not 0 < count.value <= goal:
                    raise ConnectionError('Control pipe peer disconnected')
                return count.value if kind == 'write' else buffer.raw[:count.value]

    def send(self, data):
        return self._transfer('write', data)

    def recv(self, count):
        return self._transfer('read', count)

    def recvmsg(self, count, *_):
        return self.recv(count), [], 0, None

    def sendmsg(self, *_):
        raise NotImplementedError('Windows native GPU handle transport is unavailable')

    def shutdown(self, *_):
        self.close()

    def close(self):
        global _CHANNELS
        if not hasattr(self, '_stopping'):
            return
        self._stopping.set()
        with self._lock:
            if self._pending is not None and self._handle:
                operation = self._pending[3]
                self._kernel.CancelIoEx(self._handle, ctypes.byref(operation))
                self._kernel.WaitForSingleObject(operation.event, 1000)
                count = ctypes.c_uint32()
                complete = self._kernel.GetOverlappedResult(self._handle, ctypes.byref(operation), ctypes.byref(count), False)
                if not complete and ctypes.get_last_error() == _INCOMPLETE:
                    # These owners remain charged against the same process-wide
                    # limit. No pending native OVERLAPPED/backing buffer is freed.
                    # The slot and lease were allocated before admitting IO.
                    # Preservation itself cannot fail on an allocating append.
                    _OWNERS[self._slot] = self._lease
                    self._handle = self._pending = None
                    self._charged = False
                    return
                self._kernel.CloseHandle(operation.event)
                self._lease[1] = self._pending = None
            if self._handle:
                self._kernel.CloseHandle(self._handle)
                self._lease[0] = self._handle = None
            if self._charged:
                with _BUDGET_LOCK:
                    _CHANNELS -= 1
                    _OWNERS[self._slot] = None
                    self._charged = False

    def __del__(self):
        try:
            self.close()
        except BaseException:
            pass
