"""Send the standard X11 close request to one identified test process."""
import ctypes
from ctypes import util
import os


class ClientMessageData(ctypes.Union):
    _fields_ = [('bytes', ctypes.c_byte * 20), ('shorts', ctypes.c_short * 10), ('longs', ctypes.c_long * 5)]


class ClientMessage(ctypes.Structure):
    _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong), ('send_event', ctypes.c_int),
                ('display', ctypes.c_void_p), ('window', ctypes.c_ulong), ('message_type', ctypes.c_ulong),
                ('format', ctypes.c_int), ('data', ClientMessageData)]


class Event(ctypes.Union):
    _fields_ = [('client', ClientMessage), ('padding', ctypes.c_long * 24)]


def close_process_window(pid):
    library = util.find_library('X11')
    if not library:
        raise RuntimeError('The X11 test requires libX11')
    x11 = ctypes.CDLL(library)
    display_type, window_type, atom_type = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong
    x11.XOpenDisplay.argtypes, x11.XOpenDisplay.restype = (ctypes.c_char_p,), display_type
    x11.XCloseDisplay.argtypes = (display_type,)
    x11.XDefaultRootWindow.argtypes, x11.XDefaultRootWindow.restype = (display_type,), window_type
    x11.XInternAtom.argtypes, x11.XInternAtom.restype = (display_type, ctypes.c_char_p, ctypes.c_int), atom_type
    x11.XQueryTree.argtypes = (display_type, window_type, ctypes.POINTER(window_type), ctypes.POINTER(window_type),
                              ctypes.POINTER(ctypes.POINTER(window_type)), ctypes.POINTER(ctypes.c_uint))
    x11.XQueryTree.restype = ctypes.c_int
    x11.XGetWindowProperty.argtypes = (display_type, window_type, atom_type, ctypes.c_long, ctypes.c_long, ctypes.c_int,
                                      atom_type, ctypes.POINTER(atom_type), ctypes.POINTER(ctypes.c_int),
                                      ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                      ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)))
    x11.XGetWindowProperty.restype = ctypes.c_int
    x11.XGetWMProtocols.argtypes = (display_type, window_type, ctypes.POINTER(ctypes.POINTER(atom_type)),
                                   ctypes.POINTER(ctypes.c_int))
    x11.XGetWMProtocols.restype = ctypes.c_int
    x11.XSendEvent.argtypes = (display_type, window_type, ctypes.c_int, ctypes.c_long, ctypes.POINTER(Event))
    x11.XSendEvent.restype = ctypes.c_int
    x11.XSync.argtypes = (display_type, ctypes.c_int)
    x11.XFree.argtypes = (ctypes.c_void_p,)
    error_handler = ctypes.CFUNCTYPE(ctypes.c_int, display_type, ctypes.c_void_p)(lambda _display, _error: 0)
    x11.XSetErrorHandler.argtypes = (ctypes.c_void_p,)
    x11.XSetErrorHandler.restype = ctypes.c_void_p
    previous = x11.XSetErrorHandler(error_handler)
    display = x11.XOpenDisplay(None)
    if not display:
        x11.XSetErrorHandler(previous)
        raise RuntimeError(f'Cannot open X11 display {os.environ.get("DISPLAY", "")}')
    try:
        pid_atom = x11.XInternAtom(display, b'_NET_WM_PID', 0)
        protocol_atom = x11.XInternAtom(display, b'WM_PROTOCOLS', 0)
        close_atom = x11.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
        pending = [x11.XDefaultRootWindow(display)]
        targets = []
        while pending:
            window = pending.pop()
            root, parent = window_type(), window_type()
            children = ctypes.POINTER(window_type)()
            count = ctypes.c_uint()
            if x11.XQueryTree(display, window, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count)):
                if children:
                    pending.extend(children[index] for index in range(count.value))
                    x11.XFree(children)
            actual, value_format, count, remaining = atom_type(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong()
            data = ctypes.POINTER(ctypes.c_ubyte)()
            result = x11.XGetWindowProperty(display, window, pid_atom, 0, 1, 0, 6, ctypes.byref(actual),
                                            ctypes.byref(value_format), ctypes.byref(count), ctypes.byref(remaining),
                                            ctypes.byref(data))
            matches = (result == 0 and actual.value == 6 and value_format.value == 32 and count.value == 1 and
                       bool(data) and ctypes.cast(data, ctypes.POINTER(ctypes.c_ulong))[0] == pid)
            if data:
                x11.XFree(data)
            if not matches:
                continue
            protocols, protocol_count = ctypes.POINTER(atom_type)(), ctypes.c_int()
            if x11.XGetWMProtocols(display, window, ctypes.byref(protocols), ctypes.byref(protocol_count)):
                if protocols and any(protocols[index] == close_atom for index in range(protocol_count.value)):
                    targets.append(window)
                if protocols:
                    x11.XFree(protocols)
        if len(targets) != 1:
            raise RuntimeError(f'Expected one closeable X11 window for process {pid}, found {len(targets)}')
        event = Event()
        event.client.type = 33
        event.client.display = display
        event.client.window = targets[0]
        event.client.message_type = protocol_atom
        event.client.format = 32
        event.client.data.longs[0] = close_atom
        if not x11.XSendEvent(display, targets[0], 0, 0, ctypes.byref(event)):
            raise RuntimeError('Could not send the X11 close request')
        x11.XSync(display, 0)
    finally:
        x11.XCloseDisplay(display)
        x11.XSetErrorHandler(previous)
