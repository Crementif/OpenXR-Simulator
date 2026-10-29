import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import traceback
import winreg


from packaging_support import registry_value

def run(command, expected=0, env=None, timeout=180):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    return result


def shell_folder(identity):
    value = ctypes.create_unicode_buffer(32768)
    result = ctypes.windll.shell32.SHGetFolderPathW(None, identity, None, 0, value)
    assert result == 0, result
    return Path(value.value)


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def wait_for_uninstall(prefix, log):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                   ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    invalid = ctypes.c_void_p(-1).value
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if not (prefix / ".simxr-install").exists() and not (prefix / ".simxr-use-lock").exists():
            handle = kernel.CreateFileW(str(log), 0x80000000, 0, None, 3, 0x80, None)
            if handle != invalid:
                kernel.CloseHandle(handle)
                return
        time.sleep(0.1)
    raise AssertionError("Uninstall did not finish: " + (log.read_text(encoding="utf-8-sig") if log.exists() else "no log"))


def main():
    parser = argparse.ArgumentParser(description="Exercise the native Windows installation lifecycle")
    parser.add_argument("setup", type=Path)
    parser.add_argument("stage", type=Path)
    parser.add_argument("--previous-setup", type=Path,
                        help="Install an older package first, then verify replacement and user-file preservation")
    args = parser.parse_args()
    setup = args.setup.resolve(strict=True)
    stage = args.stage.resolve(strict=True)
    previous_setup = args.previous_setup.resolve(strict=True) if args.previous_setup else None
    app_id = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{E638AA20-956C-43A5-83AF-8E6ED5A33472}_is1"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, app_id):
            raise RuntimeError("Refusing to overwrite an existing OpenXR Simulator installation")
    except FileNotFoundError:
        pass
    programs = shell_folder(2) / "OpenXR Simulator.lnk"
    desktop = shell_folder(16) / "OpenXR Simulator.lnk"
    assert not programs.exists() and not desktop.exists(), "A shortcut already exists; refusing to replace it"
    runtimes = [(root, view, registry_value(root, view))
                for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE)
                for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY)]
    with tempfile.TemporaryDirectory(prefix="simxr-win-install-") as directory:
        root = Path(directory)
        prefix = root / "OpenXR Simulator Ω test"
        data = root / "userdata" / "settings"
        data.parent.mkdir()
        data.write_text("preserve settings and recordings")
        env = dict(os.environ, SIMXR_COMPANION_SETTINGS=str(data.parent / "companion.settings"))
        native_env = dict(env, PATH="")
        common = [str(setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", f"/DIR={prefix}"]
        uninstall = prefix / "unins000.exe"
        manifest = json.loads((stage / "bin" / "openxr_simulator.json").read_text(encoding="utf-8"))
        runtime_name = manifest["runtime"]["library_path"].removeprefix("./")
        assert runtime_name in ("openxr_simulator.dll", "libopenxr_simulator.dll")
        runtime = prefix / "bin" / runtime_name
        helper = stage / "bin" / "simxr_install_check.exe"
        children = []
        uninstalled = False
        try:
            unmanaged = root / "unmanaged"
            unmanaged.mkdir()
            (unmanaged / "keep.txt").write_text("preserve")
            run([str(setup), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", f"/DIR={unmanaged}"], 7, env)
            assert (unmanaged / "keep.txt").read_text() == "preserve"
            run(common + ["/CLOSEAPPLICATIONS"], 1, env)
            run([str(previous_setup)] + common[1:] if previous_setup else common, env=env)
            assert uninstall.exists() and programs.exists() and not desktop.exists()
            user_file = prefix / "user-created" / "notes.txt"
            user_file.parent.mkdir()
            user_file.write_text("preserve extra files")
            if previous_setup:
                assert digest(runtime) != digest(stage / "bin" / runtime_name), "Previous runtime must differ"
                run(common, env=env)
                assert user_file.read_text() == "preserve extra files"
                assert data.read_text() == "preserve settings and recordings"
                assert all(registry_value(root_key, view) == previous for root_key, view, previous in runtimes)
            for file in ("bin/simxr_companion.exe", "bin/simxr_mcp_connector.exe", "bin/simxr_runner.exe",
                         f"bin/{runtime_name}", "bin/openxr_simulator.json"):
                assert digest(prefix / file) == digest(stage / file), file
            assert "simxr_companion" in run([str(prefix / "bin" / "simxr_companion.exe"), "--version"], env=native_env).stdout
            for loader_name in ("openxr_loader.dll", "libopenxr_loader.dll"):
                loader_dll = prefix / "bin" / loader_name
                if loader_dll.exists():
                    run([sys.executable, "-c", "import ctypes,sys; ctypes.WinDLL(sys.argv[1])",
                         str(loader_dll)], env=native_env)
            lock = prefix / ".simxr-use-lock"
            saved_lock = prefix / ".simxr-use-lock-test"
            lock.rename(saved_lock)
            try:
                assert "being updated" in run([str(prefix / "bin" / "simxr_companion.exe"), "--version"], 1, native_env).stderr
                run(common, 7, env)
            finally:
                saved_lock.rename(lock)

            status, stop = root / "guard.status", root / "guard.stop"
            guard = subprocess.Popen([str(helper), "--guard", str(prefix), str(os.getpid()), str(status), str(stop)],
                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(guard)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                assert guard.poll() is None, guard.stderr.read()
                if status.exists() and status.read_text().startswith("ready:"):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError("Installation guard did not become ready")
            for name in ("simxr_companion.exe", "simxr_mcp_connector.exe", "simxr_runner.exe",
                         "simxr_headset_host.exe", "xr_probe_vk.exe"):
                app_env = env if name in ("simxr_headset_host.exe", "xr_probe_vk.exe") else native_env
                blocked = run([str(prefix / "bin" / name), "--help"], 1, app_env)
                assert "being updated" in blocked.stderr, (name, blocked.stderr)
            try:
                ctypes.WinDLL(str(runtime))
                raise AssertionError("A runtime loaded while the installer held its guard")
            except OSError:
                pass
            stop.write_text("stop")
            guard.wait(timeout=10)
            assert guard.returncode == 0 and status.read_text() == "released"
            children.remove(guard)

            companion = subprocess.Popen([str(prefix / "bin" / "simxr_companion.exe"), "--windowless"], env=native_env,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(companion)
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                assert companion.poll() is None, companion.stderr.read()
                result = subprocess.run([str(helper), str(prefix)], capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
                if result.returncode == 2:
                    break
                assert result.returncode == 0, (result.returncode, result.stderr)
                time.sleep(0.1)
            else:
                raise AssertionError("Restart Manager did not detect the running companion")
            run(common, 7, env)
            busy_log = root / "uninstall-busy.log"
            run([str(uninstall), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={busy_log}"], 1, env)
            assert "Close OpenXR Simulator" in busy_log.read_text(encoding="utf-8-sig")
            assert companion.poll() is None and uninstall.exists()
            companion.terminate()
            companion.wait(timeout=10)
            children.remove(companion)

            loader_code = "import ctypes,sys; ctypes.WinDLL(sys.argv[1]); print('ready',flush=True); sys.stdin.read()"
            loader = subprocess.Popen([sys.executable, "-u", "-c", loader_code, str(runtime)], env=env,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True, creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(loader)
            assert loader.stdout.readline().strip() == "ready"
            run([str(helper), str(prefix)], 2, env)
            run(common, 7, env)
            run([str(uninstall), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={busy_log}"], 1, env)
            assert "Close OpenXR Simulator" in busy_log.read_text(encoding="utf-8-sig")
            assert loader.poll() is None and runtime.exists()
            loader.stdin.close()
            loader.wait(timeout=10)
            children.remove(loader)

            run(common + ["/TASKS=desktopicon"], env=env)
            assert programs.exists() and desktop.exists() and data.read_text() == "preserve settings and recordings"
            assert user_file.read_text() == "preserve extra files"
            final_log = root / "uninstall-final.log"
            run([str(uninstall), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={final_log}"], env=env)
            wait_for_uninstall(prefix, final_log)
            uninstalled = True
            assert not programs.exists() and not desktop.exists() and data.exists()
            assert user_file.read_text() == "preserve extra files"
            assert not (prefix / ".simxr-install").exists() and not (prefix / ".simxr-use-lock").exists(), (
                (prefix / ".simxr-install").exists(), (prefix / ".simxr-use-lock").exists(),
                final_log.read_text(encoding="utf-8-sig"))
            assert all(registry_value(root_key, view) == previous for root_key, view, previous in runtimes)
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, app_id):
                    raise AssertionError("Uninstall registration was not removed")
            except FileNotFoundError:
                pass
        except Exception:
            traceback.print_exc()
            raise
        finally:
            for child in children:
                child.terminate()
                child.wait(timeout=10)
            if not uninstalled and uninstall.exists():
                cleanup_log = stage.parent / "uninstall-cleanup.log"
                cleanup = subprocess.run([str(uninstall), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                          f"/LOG={cleanup_log}"], capture_output=True, text=True, env=env,
                                         timeout=180, creationflags=subprocess.CREATE_NO_WINDOW)
                if cleanup.returncode:
                    print("Cleanup failed:", cleanup.returncode, cleanup.stdout, cleanup.stderr,
                          cleanup_log.read_text(encoding="utf-8-sig") if cleanup_log.exists() else "no log", flush=True)
    print("PASS Windows silent install, in-use refusal, upgrade, shortcuts, uninstall and runtime selection")


if __name__ == "__main__":
    main()
