#!/usr/bin/env python3
"""Run an extracted and relocated Windows ZIP without Python or Node on PATH."""
import argparse
import base64
import ctypes
from ctypes import wintypes
from io import BytesIO
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import winreg
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'app'))
from mcp_support import Client, eventually

from mcp_support import kernel, process_handle

from packaging_support import registry_value


def loaded_modules(pid):
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    psapi.EnumProcessModulesEx.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                          ctypes.POINTER(wintypes.DWORD), wintypes.DWORD)
    psapi.EnumProcessModulesEx.restype = wintypes.BOOL
    psapi.GetModuleFileNameExW.argtypes = (wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
    psapi.GetModuleFileNameExW.restype = wintypes.DWORD
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        modules = (wintypes.HMODULE * 2048)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3):
            raise ctypes.WinError(ctypes.get_last_error())
        if needed.value > ctypes.sizeof(modules):
            raise RuntimeError('Too many probe modules to verify')
        paths = []
        for module in modules[:needed.value // ctypes.sizeof(wintypes.HMODULE)]:
            path = ctypes.create_unicode_buffer(32768)
            if not psapi.GetModuleFileNameExW(handle, module, path, len(path)):
                raise ctypes.WinError(ctypes.get_last_error())
            paths.append(Path(path.value).resolve())
        return paths
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--skip-xr', action='store_true', help='Skip GPU-dependent XR and image checks')
    parser.add_argument('--managed', action='store_true', help='Also exercise the installation-use guard')
    parser.add_argument('--manifest-alias-tool', type=Path, help='Exercise the compatibility manifest with a real loader')
    args = parser.parse_args()
    archive = args.archive.resolve(strict=True)
    registries = [(root, view, registry_value(root, view))
                  for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE)
                  for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY)]
    with tempfile.TemporaryDirectory(prefix='simxr-windows-portable-') as temporary:
        root = Path(temporary)
        extraction = root / 'original'
        extraction.mkdir()
        with zipfile.ZipFile(archive) as zip_file:
            for item in zip_file.infolist():
                name = item.filename.replace('\\', '/')
                assert not name.startswith('/') and all(part not in ('..', '') for part in name.rstrip('/').split('/')), name
                assert ':' not in name, name
            zip_file.extractall(extraction)
        items = list(extraction.iterdir())
        assert len(items) == 1 and items[0].is_dir(), items
        relocated = root / 'moved Ω $' / 'OpenXR Simulator'
        relocated.parent.mkdir()
        shutil.move(str(items[0]), str(relocated))
        if args.managed:
            (relocated / '.simxr-install').write_text('openxr-simulator-install-v2\n', encoding='ascii')
            (relocated / '.simxr-use-lock').touch()
        binary = relocated / 'bin'
        connector = binary / 'simxr_mcp_connector.exe'
        helper = root / 'simxr-install-check.exe'
        if args.managed:
            shutil.copy2(binary / 'simxr_install_check.exe', helper)
        manifest = json.loads((binary / 'openxr_simulator.json').read_text(encoding='utf-8'))
        runtime_name = manifest['runtime']['library_path']
        assert runtime_name in ('./openxr_simulator.dll', './libopenxr_simulator.dll'), runtime_name
        runtime = binary / runtime_name[2:]
        assert runtime.exists() and (binary / 'openxr_loader.dll').exists()
        marker = root / 'preserved settings Ω.txt'
        marker.write_text('preserve user data', encoding='utf-8')
        env = dict(os.environ, PATH=str(Path(os.environ['SystemRoot']) / 'System32'),
                   SIMXR_DATA_DIR=str(root / 'data'), SIMXR_MCP_DISCOVERY_DIR=str(root / 'discovery'),
                   SIMXR_COMPANION_SETTINGS=str(root / 'settings'), SDL_VIDEODRIVER='simxr-unavailable-video-driver')
        for key in ('SIMXR_MCP_TOKEN', 'XR_RUNTIME_JSON', 'PYTHONPATH'):
            env.pop(key, None)
        config = subprocess.run([str(connector), '--print-config', '--windowless'], env=env, cwd=root,
                                capture_output=True, check=True, timeout=15)
        configured = json.loads(config.stdout)['mcpServers']['openxr-simulator']
        assert Path(configured['command']).resolve() == connector.resolve() and configured['args'] == ['--windowless']
        assert not (root / 'discovery').exists()
        handles = []
        bridge = None
        with (root / 'host.log').open('wb') as log:
            try:
                bridge = subprocess.Popen([configured['command'], *configured['args']], env=env, cwd=root,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8', bufsize=1,
                    creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                client = Client(bridge)
                assert 'result' in client.rpc('initialize', {'protocolVersion': '2025-11-25', 'capabilities': {},
                    'clientInfo': {'name': 'windows-portable-test', 'version': '1'}})
                client.send({'jsonrpc': '2.0', 'method': 'notifications/initialized'})
                names = {entry['name'] for entry in client.rpc('tools/list')['result']['tools']}
                assert {'simulator_launch_application', 'simulator_luau_run', 'capture_screenshot'} <= names
                discovery = eventually(lambda: next((p for p in (root / 'discovery').glob('*.json')), None))
                handles.append(process_handle(int(json.loads(discovery.read_text(encoding='utf-8'))['process_id'])))
                if not args.skip_xr:
                    launch = {'executable': str(binary / 'xr_probe_vk.exe'),
                        'environment': {'SIMXR_PROBE_FRAMES': '120000', 'SIMXR_PROBE_SLEEP_MS': '1',
                            'DISABLE_XR_APILAYER_NOVENDOR_OBSMirror': '1',
                            'DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY': '1'}}
                    if args.manifest_alias_tool:
                        alias = subprocess.run([str(args.manifest_alias_tool.resolve(strict=True)),
                                                str(binary / 'openxr_simulator.json')], env=env, cwd=root,
                                               capture_output=True, check=True, timeout=15).stdout.decode('ascii')
                        assert alias.isascii() and Path(alias).is_file(), alias
                        launch['runtime_manifest'] = alias
                    launched = client.tool('simulator_launch_application', launch)
                    assert not launched['error'], launched
                    application = launched['data']['application_id']
                    def process_started():
                        status = client.tool('simulator_application_status', {'application_id': application})
                        assert not status['error'] and status['data']['launch_state'] != 'failed', status
                        return status['data'] if int(status['data']['process_id']) else None
                    started = eventually(process_started, timeout=30)
                    handles.append(process_handle(int(started['process_id'])))
                    def connected():
                        status = client.tool('simulator_application_status', {'application_id': application})
                        assert not status['error'] and status['data']['launch_state'] not in ('failed', 'exited'), status
                        return status['data'] if status['data']['session_running'] else None
                    state = eventually(connected, timeout=45)
                    assert state['process_id'] == started['process_id'], state
                    assert runtime.resolve() in loaded_modules(int(state['process_id']))
                    script = client.tool('simulator_luau_run', {'application_id': application,
                        'source': "simxr.checkpoint('portable run', true)"})
                    assert not script['error'], script
                    def completed():
                        value = client.tool('simulator_luau_status', {'application_id': application, 'run_id': script['data']['run_id']})
                        assert not value['error'], value
                        return value['data'] if value['data']['phase'] not in ('running', 'paused') else None
                    outcome = eventually(completed, timeout=30)
                    assert outcome['phase'] == 'complete' and outcome['checkpoints'][0]['passed'], outcome
                    def screenshot():
                        result = client.rpc('tools/call', {'name': 'capture_screenshot', 'arguments': {
                            'application_id': application, 'eye': 'left', 'format': 'png', 'max_width': 64, 'timeout': 20}})['result']
                        if result['isError'] and 'unavailable' in result['content'][0]['text']:
                            return None
                        assert not result['isError'], result
                        return result
                    image_result = eventually(screenshot, timeout=45)['content'][1]
                    assert image_result['type'] == 'image' and image_result['mimeType'] == 'image/png'
                    from PIL import Image
                    with Image.open(BytesIO(base64.b64decode(image_result['data'], validate=True))) as image:
                        image.load()
                        assert image.format == 'PNG' and image.width == 64 and image.height > 0
                        assert max(channel[1] for channel in image.convert('RGB').getextrema()) > 0
                assert not client.tool('simulator_shutdown', {})['error']
                bridge.stdin.close()
                assert bridge.wait(timeout=15) == 0
                assert marker.read_text(encoding='utf-8') == 'preserve user data'
                if not args.skip_xr:
                    assert kernel.WaitForSingleObject(handles[-1], 0) == 0x102, 'Shutdown unexpectedly terminated the game'
                    if args.managed:
                        assert kernel.WaitForSingleObject(handles[0], 15000) == 0, 'Simulator host did not exit'
                        guard = subprocess.run([str(helper), str(relocated)], env=env,
                            capture_output=True, timeout=15)
                        assert guard.returncode == 2, (guard.returncode, guard.stdout, guard.stderr)
                        kernel.TerminateProcess(handles[-1], 1)
                        assert kernel.WaitForSingleObject(handles[-1], 10000) == 0
                        guard = subprocess.run([str(helper), str(relocated)], env=env,
                            capture_output=True, timeout=15)
                        assert guard.returncode == 0, (guard.returncode, guard.stdout, guard.stderr)
                for registry_root, view, before in registries:
                    assert registry_value(registry_root, view) == before, 'System OpenXR registration changed'
            finally:
                if bridge and bridge.poll() is None:
                    bridge.kill()
                    bridge.wait(timeout=10)
                if not handles:
                    deadline = time.monotonic() + 5
                    while not handles and time.monotonic() < deadline:
                        for record in (root / 'discovery').glob('*.json'):
                            try:
                                handles.append(process_handle(int(json.loads(record.read_text(encoding='utf-8'))['process_id'])))
                            except (OSError, KeyError, ValueError, json.JSONDecodeError):
                                pass
                        if not handles:
                            time.sleep(.05)
                for handle in reversed(handles):
                    if kernel.WaitForSingleObject(handle, 0) == 0x102:
                        kernel.TerminateProcess(handle, 1)
                        kernel.WaitForSingleObject(handle, 10000)
                    kernel.CloseHandle(handle)
    print('PASS relocated Windows ZIP and generated MCP configuration' +
              ('; XR, Luau and image checks skipped' if args.skip_xr else ', XR launch, Luau and PNG'))


if __name__ == '__main__':
    main()
