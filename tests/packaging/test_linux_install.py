#!/usr/bin/env python3
import os
import fcntl
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import shutil


def run(command, env, expected=0):
    result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=30)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    return result


def main():
    stage = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="simxr-install-test-") as directory:
        root = Path(directory)
        home = root / "home"
        home.mkdir()
        data = home / "user data"
        prefix = home / 'apps Ω % $ "' / "OpenXR Simulator"
        env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(data))
        installer = stage / "install.sh"
        args = [str(installer), "--prefix", str(prefix)]
        settings = home / ".config" / "simxr" / "settings"
        settings.parent.mkdir(parents=True)
        settings.write_text("keep this profile")
        take = data / "simxr" / "takes" / "take.simxr"
        take.parent.mkdir(parents=True)
        take.write_text("keep this recording")
        runtime_selection = home / ".config" / "openxr" / "1" / "active_runtime.json"
        runtime_selection.parent.mkdir(parents=True)
        runtime_selection.write_text("existing runtime selection")
        runtime_dir = root / "runtime"
        runtime_dir.mkdir(mode=0o700)
        env["XDG_RUNTIME_DIR"] = str(runtime_dir)
        run(args, env)
        assert (prefix / "bin" / "simxr_mcp_connector").is_file()
        assert (prefix / "bin" / "simxr_runner").is_file()
        assert not (prefix / "share" / "simxr" / "sdk" / "python" / "build").exists()
        assert "simxr_companion" in run([str(prefix / "bin" / "simxr_companion"), "--version"], env).stdout
        with (prefix / ".simxr-use-lock").open("r") as exclusive:
            fcntl.flock(exclusive, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for name in ("simxr_companion", "simxr_mcp_connector", "simxr_runner", "simxr_headset_host", "xr_probe_linux"):
                program = prefix / "bin" / name
                if program.exists():
                    unavailable = run([str(program), "--help"], env, 1)
                    assert "being updated" in unavailable.stderr, (name, unavailable.stderr)
            fcntl.flock(exclusive, fcntl.LOCK_UN)
        desktop = data / "applications" / "openxr-simulator.desktop"
        run(["desktop-file-validate", str(desktop)], env)
        text = desktop.read_text()
        assert "Name=OpenXR Simulator" in text and "%%" in text and f"X-Simxr-Install={prefix}" in text

        companion = subprocess.Popen([str(prefix / "bin" / "simxr_companion"), "--windowless"], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while True:
                assert companion.poll() is None, companion.stderr.read()
                with (prefix / ".simxr-use-lock").open("r") as probe:
                    try:
                        fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        break
                assert time.monotonic() < deadline, "Companion did not acquire the installation lock"
                time.sleep(0.05)
            assert "running" in run(args, env, 2).stderr
            assert companion.poll() is None
        finally:
            companion.terminate()
            companion.wait(timeout=10)

        for mode in ("normal", "nondumpable", "portable_first"):
            code = "import ctypes,sys; "
            if mode == "portable_first":
                code += "ctypes.CDLL(sys.argv[2],mode=ctypes.RTLD_GLOBAL); "
            code += "ctypes.CDLL(sys.argv[1]); "
            if mode == "nondumpable":
                code += "assert ctypes.CDLL(None).prctl(4,0,0,0,0) == 0; "
            code += "print('ready',flush=True); sys.stdin.read()"
            loader = subprocess.Popen([sys.executable, "-u", "-c", code,
                                       str(prefix / "bin" / "libopenxr_simulator.so"),
                                       str(stage / "bin" / "libopenxr_simulator.so")], env=env,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                assert loader.stdout.readline().strip() == "ready"
                upgrade = run(args, env, 2)
                removal = run(args + ["--uninstall"], env, 2)
                assert all("running" in result.stderr for result in (upgrade, removal)), (upgrade.stderr, removal.stderr)
                assert loader.poll() is None and (prefix / "bin" / "libopenxr_simulator.so").exists()
            finally:
                loader.stdin.close()
                loader.wait(timeout=10)
        run(args, env)
        assert settings.read_text() == "keep this profile" and take.read_text() == "keep this recording"
        assert runtime_selection.read_text() == "existing runtime selection"
        run([str(prefix / "install.sh"), "--prefix", str(prefix), "--uninstall"], env)
        assert not prefix.exists() and not desktop.exists() and settings.read_text() == "keep this profile"
        assert take.read_text() == "keep this recording" and runtime_selection.read_text() == "existing runtime selection"

        unmanaged = home / "unmanaged"
        unmanaged.mkdir()
        (unmanaged / "important").write_text("keep")
        run([str(installer), "--prefix", str(unmanaged)], env, 1)
        assert (unmanaged / "important").read_text() == "keep"

        mock = root / "mock-package"
        (mock / "bin").mkdir(parents=True)
        (mock / "share" / "simxr" / "branding").mkdir(parents=True)
        shutil.copy2(installer, mock / "install.sh")
        shutil.copy2(stage / "launch-desktop.sh", mock / "launch-desktop.sh")
        (mock / "bin" / "openxr_simulator.json").write_text("{}")
        (mock / "share" / "simxr" / "branding" / "openxr-simulator.png").write_bytes(b"icon")
        fake = mock / "bin" / "simxr_companion"
        fake.write_text('#!/bin/sh\nprintf started > "$SIMXR_LAUNCH_MARKER"\n')
        fake.chmod(0o755)
        marker = root / "launched"
        env["SIMXR_LAUNCH_MARKER"] = str(marker)
        run([str(mock / "install.sh"), "--prefix", str(prefix)], env)
        run(["gio", "launch", str(desktop)], env)
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert marker.read_text() == "started"
        run([str(mock / "install.sh"), "--prefix", str(prefix), "--uninstall"], env)
    print("PASS Linux staged install, in-use refusal, upgrade, desktop entry and uninstall")


if __name__ == "__main__":
    main()
