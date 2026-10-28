#!/usr/bin/env python3
"""Check an actionable visible startup failure and noninteractive automation."""

import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

from test_mcp_connector import Client


class WindowAttributes(ctypes.Structure):
    _fields_ = [('x', ctypes.c_int), ('y', ctypes.c_int), ('width', ctypes.c_int), ('height', ctypes.c_int),
                ('border_width', ctypes.c_int), ('depth', ctypes.c_int), ('visual', ctypes.c_void_p),
                ('root', ctypes.c_ulong), ('class_', ctypes.c_int), ('bit_gravity', ctypes.c_int),
                ('win_gravity', ctypes.c_int), ('backing_store', ctypes.c_int), ('backing_planes', ctypes.c_ulong),
                ('backing_pixel', ctypes.c_ulong), ('save_under', ctypes.c_int), ('colormap', ctypes.c_ulong),
                ('map_installed', ctypes.c_int), ('map_state', ctypes.c_int), ('all_event_masks', ctypes.c_long),
                ('your_event_mask', ctypes.c_long), ('do_not_propagate_mask', ctypes.c_long),
                ('override_redirect', ctypes.c_int), ('screen', ctypes.c_void_p)]


class TextProperty(ctypes.Structure):
    _fields_ = [('value', ctypes.POINTER(ctypes.c_ubyte)), ('encoding', ctypes.c_ulong),
                ('format', ctypes.c_int), ('nitems', ctypes.c_ulong)]


def main():
    app = str(Path(sys.argv[1]).resolve(strict=True))
    connector = str(Path(sys.argv[2]).resolve(strict=True))
    x11 = ctypes.CDLL('libX11.so.6')
    xtest = ctypes.CDLL('libXtst.so.6')
    pointer = ctypes.c_void_p
    window = ctypes.c_ulong
    x11.XOpenDisplay.argtypes, x11.XOpenDisplay.restype = [ctypes.c_char_p], pointer
    x11.XDefaultRootWindow.argtypes, x11.XDefaultRootWindow.restype = [pointer], window
    x11.XDefaultScreen.argtypes, x11.XDefaultScreen.restype = [pointer], ctypes.c_int
    x11.XDisplayWidth.argtypes, x11.XDisplayWidth.restype = [pointer, ctypes.c_int], ctypes.c_int
    x11.XDisplayHeight.argtypes, x11.XDisplayHeight.restype = [pointer, ctypes.c_int], ctypes.c_int
    x11.XQueryTree.argtypes = [pointer, window, ctypes.POINTER(window), ctypes.POINTER(window),
                              ctypes.POINTER(ctypes.POINTER(window)), ctypes.POINTER(ctypes.c_uint)]
    x11.XGetGeometry.argtypes = [pointer, window, ctypes.POINTER(window), ctypes.POINTER(ctypes.c_int),
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint),
                                ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    x11.XFree.argtypes = [pointer]
    x11.XGetWindowAttributes.argtypes = [pointer, window, ctypes.POINTER(WindowAttributes)]
    x11.XGetWMName.argtypes = [pointer, window, ctypes.POINTER(TextProperty)]
    handler_type = ctypes.CFUNCTYPE(ctypes.c_int, pointer, pointer)
    error_handler = handler_type(lambda _display, _error: 0)
    x11.XSetErrorHandler.argtypes = [handler_type]
    x11.XSetErrorHandler(error_handler)
    x11.XSetInputFocus.argtypes = [pointer, window, ctypes.c_int, window]
    x11.XKeysymToKeycode.argtypes, x11.XKeysymToKeycode.restype = [pointer, window], ctypes.c_ubyte
    x11.XFlush.argtypes = [pointer]
    x11.XCloseDisplay.argtypes = [pointer]
    xtest.XTestFakeKeyEvent.argtypes = [pointer, ctypes.c_uint, ctypes.c_int, window]
    display = x11.XOpenDisplay(None)
    assert display, 'This test needs a private X11 display'
    try:
        root = x11.XDefaultRootWindow(display)
        screen = x11.XDefaultScreen(display)

        def windows():
            root_return, parent = window(), window()
            children, count = ctypes.POINTER(window)(), ctypes.c_uint()
            assert x11.XQueryTree(display, root, ctypes.byref(root_return), ctypes.byref(parent),
                                  ctypes.byref(children), ctypes.byref(count))
            try:
                return [children[index] for index in range(count.value)]
            finally:
                if children:
                    x11.XFree(children)

        assert not windows(), 'Use an isolated display with no other windows'
        with tempfile.TemporaryDirectory(prefix='simxr-startup-failure-') as temporary:
            private = Path(temporary)
            env = dict(os.environ, HOME=temporary, PATH='', XDG_RUNTIME_DIR=temporary,
                       SIMXR_DATA_DIR=temporary, SIMXR_COMPANION_SETTINGS=str(private / 'settings'),
                       SDL_VIDEODRIVER='x11', VK_DRIVER_FILES=str(private / 'missing-icd.json'),
                       DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent')
            for key in ('WAYLAND_DISPLAY', 'XR_RUNTIME_JSON', 'SIMXR_MCP_TOKEN', 'VK_ICD_FILENAMES'):
                env.pop(key, None)
            for arguments in (['--hidden'], ['--mcp'], ['--mcp-http', '0']):
                result = subprocess.run([app, *arguments], env=env, cwd=temporary, text=True,
                                        capture_output=True, timeout=15)
                assert result.returncode == 1 and 'Vulkan' in result.stderr, result
                assert not windows(), 'An automated launch opened an error dialog'
            bridge = subprocess.Popen([connector, '--app', app], env=env, cwd=temporary, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
                                      start_new_session=True)
            try:
                client = Client(bridge)
                response = client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                                      'clientInfo': {'name': 'startup-test', 'version': '1'}})
                assert 'error' in response and 'exited before MCP became ready' in response['error']['message'], response
                assert not windows(), 'Connector startup opened an error dialog'
            finally:
                if bridge.poll() is None:
                    bridge.stdin.close()
                    try:
                        bridge.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(bridge.pid, signal.SIGKILL)
                        bridge.wait(timeout=10)
            process = subprocess.Popen([app], env=env, cwd=temporary, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                deadline = time.monotonic() + 15
                dialog = None
                while dialog is None:
                    assert process.poll() is None, process.communicate()
                    assert time.monotonic() < deadline, 'No startup error dialog appeared'
                    for candidate in windows():
                        attributes, title = WindowAttributes(), TextProperty()
                        if not x11.XGetWindowAttributes(display, candidate, ctypes.byref(attributes)) or attributes.map_state != 2:
                            continue
                        if not x11.XGetWMName(display, candidate, ctypes.byref(title)) or not title.value:
                            continue
                        try:
                            if ctypes.string_at(title.value, title.nitems) == b'OpenXR Simulator':
                                dialog = candidate
                                break
                        finally:
                            x11.XFree(title.value)
                    time.sleep(.05)
                screen_root, x, y = window(), ctypes.c_int(), ctypes.c_int()
                width, height, border, depth = (ctypes.c_uint() for _ in range(4))
                assert x11.XGetGeometry(display, dialog, ctypes.byref(screen_root), ctypes.byref(x), ctypes.byref(y),
                                        ctypes.byref(width), ctypes.byref(height), ctypes.byref(border), ctypes.byref(depth))
                assert x.value >= 0 and y.value >= 0 and x.value + width.value <= x11.XDisplayWidth(display, screen)
                assert y.value + height.value <= x11.XDisplayHeight(display, screen)
                x11.XSetInputFocus(display, dialog, 2, 0)
                enter = x11.XKeysymToKeycode(display, 0xff0d)
                assert enter and xtest.XTestFakeKeyEvent(display, enter, 1, 0)
                assert xtest.XTestFakeKeyEvent(display, enter, 0, 0)
                x11.XFlush(display)
                _, error = process.communicate(timeout=15)
                assert process.returncode == 1 and 'Vulkan' in error and 'graphics driver' in error, error
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=10)
    finally:
        x11.XCloseDisplay(display)
    print('PASS visible Vulkan startup error and noninteractive hidden/MCP/connector failures')


if __name__ == '__main__':
    main()
