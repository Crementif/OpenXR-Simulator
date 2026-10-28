import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    companion, fixture = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="simxr profile ' 日本語 ") as temporary:
        root = Path(temporary)
        settings = root / "settings"
        (root / "runtime.json").write_text("{}", encoding="utf-8")
        command = subprocess.check_output(
            [fixture, "--profile-fixture", str(settings), str(root), companion], text=True
        )
        environment = dict(os.environ, SIMXR_COMPANION_SETTINGS=str(settings), SIMXR_DATA_DIR=str(root))
        name = "--game '\\ \" 日本語 $(touch injected)"
        invocations = (["/bin/sh", "-c", command + " --hidden"],
                       [companion, "--launch-profile-hex=" + name.encode().hex(), "--hidden"])
        for invocation in invocations:
            result = root / "result.txt"
            result.unlink(missing_ok=True)
            process = subprocess.Popen(
                invocation, cwd=root, env=environment, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 15
                expected = [str(root / "runtime.json"), "literal value", "generic", "argument with spaces"]
                while time.monotonic() < deadline:
                    if result.exists() and result.read_text(encoding="utf-8").splitlines() == expected:
                        break
                    if process.poll() is not None:
                        raise AssertionError("Simulator exited: " + process.stderr.read().decode(errors="replace"))
                    time.sleep(0.05)
                assert result.exists(), "Saved profile did not launch"
                assert result.read_text(encoding="utf-8").splitlines() == expected
                assert not (root / "injected").exists(), "Profile text was interpreted by the shell"
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                process.stderr.close()

        missing = subprocess.run(
            [companion, "--launch-profile=missing"], env=environment, capture_output=True, text=True
        )
        assert missing.returncode != 0 and "Saved launch profile not found" in missing.stderr
        invalid = subprocess.run(
            [companion, "--launch-profile=one", "--windowless"], env=environment, capture_output=True, text=True
        )
        assert invalid.returncode == 2

        go = root / "go"
        ready = [root / "ready1", root / "ready2"]
        writers = [subprocess.Popen(
            [fixture, "--settings-race-fixture", str(settings), str(ready[index]), str(go), str(index)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        ) for index in range(2)]
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not all(path.exists() for path in ready):
                time.sleep(0.02)
            assert all(path.exists() for path in ready), "Settings writers did not become ready"
            go.touch()
            outcomes = []
            for writer in writers:
                _, error = writer.communicate(timeout=10)
                if writer.returncode == 3:
                    assert b"Settings changed in another simulator window" in error
                outcomes.append(writer.returncode)
            assert sorted(outcomes) == [0, 3], f"Settings race outcomes: {outcomes}"
        finally:
            for writer in writers:
                if writer.poll() is None:
                    writer.kill()
                    writer.wait()
    print("PASS saved profile command")


if __name__ == "__main__":
    main()
