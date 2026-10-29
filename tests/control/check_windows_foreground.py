"""Check keyboard focus routing against a running Windows OpenXR application.

Run in an interactive desktop session with input_polling=app and no other input
owner. This test briefly focuses the selected game and a temporary test window.
"""
import ctypes
import time
import json
from ctypes import wintypes as w

user = ctypes.WinDLL('user32', use_last_error=True)
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = (callback, w.LPARAM)
user.GetWindowThreadProcessId.argtypes = (w.HWND, ctypes.POINTER(w.DWORD))
user.GetWindowThreadProcessId.restype = w.DWORD
user.GetForegroundWindow.restype = w.HWND
user.SetForegroundWindow.argtypes = (w.HWND,)
user.ShowWindow.argtypes = (w.HWND, ctypes.c_int)
user.IsWindowVisible.argtypes = (w.HWND,)
user.CreateWindowExW.argtypes = (w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
                              ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                              w.HWND, w.HMENU, w.HINSTANCE, ctypes.c_void_p)
user.CreateWindowExW.restype = w.HWND
user.DestroyWindow.argtypes = (w.HWND,)
user.AttachThreadInput.argtypes = (w.DWORD, w.DWORD, w.BOOL)
user.keybd_event.argtypes = (w.BYTE, w.BYTE, w.DWORD, ctypes.c_size_t)
user.GetAsyncKeyState.argtypes = (ctypes.c_int,)
user.GetAsyncKeyState.restype = ctypes.c_short
user.PeekMessageW.argtypes = (ctypes.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT)


def pump(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        msg = w.MSG()
        while user.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user.TranslateMessage(ctypes.byref(msg))
            user.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.01)


def focus(window):
    owner = w.DWORD()
    assert user.GetWindowThreadProcessId(window, ctypes.byref(owner)), 'Target window no longer exists'
    user.ShowWindow(window, 9)  # SW_RESTORE
    current = user.GetForegroundWindow()
    thread = user.GetWindowThreadProcessId(current, None)
    own = kernel.GetCurrentThreadId()
    attached = bool(thread != own and user.AttachThreadInput(own, thread, True))
    try:
        user.SetForegroundWindow(window)
    finally:
        if attached:
            user.AttachThreadInput(own, thread, False)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        foreground = w.DWORD()
        user.GetWindowThreadProcessId(user.GetForegroundWindow(), ctypes.byref(foreground))
        if foreground.value == owner.value:
            return
        pump(0.02)
    raise AssertionError(f'Foreground process {foreground.value} did not become target {owner.value}')


def check(runtime, session, pid, path):
    windows = []

    @callback
    def visit(h, l):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid and user.IsWindowVisible(h):
            windows.append(h)
        return True
    user.EnumWindows(visit, 0)
    assert windows, 'No visible game window'
    previous = user.GetForegroundWindow()
    dummy = user.CreateWindowExW(0, 'STATIC', 'Simulator acceptance: background input isolation',
                                 0x10CF0000, 40, 40, 420, 100, None, None, None, None)
    assert dummy, 'Could not create focus target'
    game = windows[0]
    held = []
    samples = []

    def sample(label):
        last = runtime.status(session)['presentation']['last_submitted']
        target = last + 4
        deadline = time.monotonic() + 10
        while last < target:
            assert time.monotonic() < deadline, f'No new applied frames for {label}'
            pump(0.02)
            last = runtime.status(session)['presentation']['last_submitted']
        rows = runtime.frame_history(session, after=max(0, last - 3), count=16)['frames']
        assert rows
        row = rows[-1]
        commands = row['applied_input']['commands']
        head = next((c for c in commands if c['kind'] == 'head_pose'))
        right = next((c for c in commands if c['kind'] == 'controller_controls' and c['hand'] == 1))
        value = {'label': label, 'frame': row['frame_id'], 'head': head, 'right': right}
        samples.append(value)
        path.write_text(json.dumps(samples, indent=2))
        return value

    def down(key):
        assert not user.GetAsyncKeyState(key) & 0x8000, 'Physical key already held'
        user.keybd_event(key, 0, 0, 0)
        held.append(key)

    def up(key):
        user.keybd_event(key, 0, 2, 0)
        held.remove(key)
    try:
        focus(game)
        before = sample('foreground_before')
        down(ord('E'))
        pump(0.3)
        moving = sample('foreground_held')
        focus(dummy)
        pump(0.3)
        background = sample('background_held')
        up(ord('E'))
        pump(0.1)
        focus(game)
        pump(0.3)
        returned = sample('foreground_released')
        assert moving['head']['position'][1] > before['head']['position'][1] + 0.05, 'Foreground key did not move the head'
        assert abs(returned['head']['position'][1] - background['head']['position'][1]) < 0.02, 'Released key leaked on return'
        focus(dummy)
        b0 = sample('background_before')
        down(ord('E'))
        pump(0.3)
        b1 = sample('background_only')
        up(ord('E'))
        assert abs(b1['head']['position'][1] - b0['head']['position'][1]) < 0.02, 'Background key moved the game'
        focus(game)
        down(0x20)
        pump(0.2)
        trigger = sample('foreground_trigger')
        focus(dummy)
        pump(0.2)
        released = sample('background_trigger')
        up(0x20)
        assert trigger['right']['trigger'] > 0.9, 'Foreground trigger did not reach runtime'
        assert released['right']['trigger'] == 0, 'Held trigger leaked after focus loss'
        print('PASS real-game foreground movement, background suppression and held-trigger release', flush=True)
    finally:
        for key in held:
            user.keybd_event(key, 0, 2, 0)
        if user.GetForegroundWindow() in (game, dummy) and previous:
            user.SetForegroundWindow(previous)
        user.DestroyWindow(dummy)

if __name__ == '__main__':
    import argparse
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk' / 'python'))
    from simxr_control import Client
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    with Client(args.socket, timeout=5) as runtime:
        session, = runtime.sessions()
        check(runtime, session, args.pid, args.output)
