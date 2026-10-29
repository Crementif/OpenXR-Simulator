#!/usr/bin/env python3
"""Run a copied launcher command through real PowerShell from a relocated package."""
import base64
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

from mcp_support import kernel


class ProcessEntry(ctypes.Structure):
    _fields_ = [('size', wintypes.DWORD), ('usage', wintypes.DWORD), ('pid', wintypes.DWORD),
                ('heap', ctypes.c_size_t), ('module', wintypes.DWORD), ('threads', wintypes.DWORD),
                ('parent', wintypes.DWORD), ('priority', wintypes.LONG), ('flags', wintypes.DWORD),
                ('name', wintypes.WCHAR * 260)]


kernel.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
kernel.Process32FirstW.restype = wintypes.BOOL
kernel.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(ProcessEntry))
kernel.Process32NextW.restype = wintypes.BOOL
kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                            ctypes.POINTER(wintypes.DWORD))
kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
kernel.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
kernel.GetExitCodeProcess.restype = wintypes.BOOL


def capture_processes(paths, handles):
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    expected = {path.resolve() for path in paths}
    names = {path.name.lower() for path in paths}
    try:
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(entry)
        found = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while found:
            if entry.name.lower() in names and entry.pid not in handles:
                handle = kernel.OpenProcess(0x00100000 | 0x0001 | 0x1000, False, entry.pid)
                if handle:
                    text = ctypes.create_unicode_buffer(32768)
                    size = wintypes.DWORD(len(text))
                    if kernel.QueryFullProcessImageNameW(handle, 0, text, ctypes.byref(size)) and Path(text.value).resolve() in expected:
                        handles[entry.pid] = handle
                    else:
                        kernel.CloseHandle(handle)
            found = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)


def main():
    archive, fixture = (Path(value).resolve(strict=True) for value in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix='simxr-profile-windows-') as temporary:
        root = Path(temporary)
        extracted = root / 'extract'
        extracted.mkdir()
        with zipfile.ZipFile(archive) as source:
            for item in source.infolist():
                name = item.filename.replace('\\', '/')
                assert not name.startswith('/') and ':' not in name and all(
                    part not in ('', '..') for part in name.rstrip('/').split('/')), name
            source.extractall(extracted)
        items = list(extracted.iterdir())
        assert len(items) == 1 and items[0].is_dir()
        package = root / "moved Ω '$(ni injected.txt) $" / 'OpenXR Simulator'
        package.parent.mkdir()
        shutil.move(str(items[0]), str(package))
        binary = package / 'bin'
        app = binary / 'simxr_companion.exe'
        child = binary / 'windows_profile_fixture.exe'
        shutil.copy2(fixture, child)
        working = root / "working Ω '$"
        working.mkdir()
        discovery = root / 'discovery'
        env = dict(os.environ, SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SIMXR_PROFILE_FIXTURE_ROOT=str(root),
                   SIMXR_PROFILE_FIXTURE_APP=str(app), SIMXR_DATA_DIR=str(root / 'data'),
                   SIMXR_MCP_DISCOVERY_DIR=str(discovery), PATH=str(Path(os.environ['SystemRoot']) / 'System32'))
        for key in ('SIMXR_MCP_TOKEN', 'SIMXR_CONTROL_SOCKET', 'XR_RUNTIME_JSON', 'PYTHONPATH'):
            env.pop(key, None)
        generated = subprocess.run([str(child), '--setup'], env=env, cwd=root, capture_output=True, check=True, timeout=10)
        command = generated.stdout.decode('utf-8')
        assert command.startswith('& ') and '--launch-profile-hex=' in command, command
        command += ' --hidden --mcp'
        powershell = Path(os.environ['SystemRoot']) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
        encoded = base64.b64encode(command.encode('utf-16le')).decode('ascii')
        error_path = root / 'powershell-stderr.log'
        error_log = error_path.open('wb')
        process = subprocess.Popen([str(powershell), '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                                   env=env, cwd=root, stdout=subprocess.DEVNULL, stderr=error_log)
        handles = {}
        try:
            result = root / 'result.txt'
            deadline = time.monotonic() + 40
            lines = []
            while time.monotonic() < deadline:
                capture_processes((app, child), handles)
                if os.environ.get('SIMXR_PROFILE_TEST_ABORT_AFTER_START') == '1' and handles:
                    raise RuntimeError('Forced failure after test process startup')
                if result.exists():
                    lines = result.read_text(encoding='utf-8').splitlines()
                    if lines and lines[-1] == 'done':
                        break
                if process.poll() not in (None, 0):
                    error_log.flush()
                    raise AssertionError('PowerShell failed: ' + error_path.read_text(encoding='utf-8', errors='replace'))
                time.sleep(0.05)
            assert lines and lines[-1] == 'done', 'The saved profile did not launch the fixture'
            pid, child_pid = int(lines[0]), int(lines[1])
            capture_processes((app, child), handles)
            assert pid in handles and child_pid in handles, (pid, child_pid, list(handles))
            assert Path(lines[2]).resolve() == working.resolve(), lines
            assert lines[3:5] == ['literal Ω $ value', 'generic'], lines
            manifest = Path(lines[5])
            manifest_json = json.loads(manifest.read_text(encoding='utf-8'))
            library = Path(manifest_json['runtime']['library_path'])
            if not library.is_absolute():
                library = manifest.parent / library
            assert library.resolve() == (binary / 'openxr_simulator.dll').resolve(), (library, binary)
            assert lines[6:] == ['5', '--child', '', 'argument with spaces', "日本語 '$(ignored)", 'done'], lines
            assert not (root / 'injected.txt').exists(), 'Profile name or executable path was interpreted by PowerShell'
            (root / 'release-child').touch()
            assert kernel.WaitForSingleObject(handles[child_pid], 10000) == 0, 'Fixture child did not exit'
            exit_code = wintypes.DWORD()
            assert kernel.GetExitCodeProcess(handles[child_pid], ctypes.byref(exit_code)) and exit_code.value == 0
            print('PASS relocated Windows PowerShell command, Unicode profile, child environment and arguments')
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            (root / 'release-child').touch()
            capture_processes((app, child), handles)
            errors = []
            for pid, handle in handles.items():
                state = kernel.WaitForSingleObject(handle, 0)
                if state == 0x102:
                    if not kernel.TerminateProcess(handle, 1) and kernel.WaitForSingleObject(handle, 0) != 0:
                        errors.append(f'Could not terminate test process {pid}')
                    if kernel.WaitForSingleObject(handle, 10000) != 0:
                        errors.append(f'Test process {pid} did not exit')
                elif state != 0:
                    errors.append(f'Could not inspect test process {pid}')
                kernel.CloseHandle(handle)
            error_log.close()
            if errors:
                raise RuntimeError('; '.join(errors))


if __name__ == '__main__':
    main()
