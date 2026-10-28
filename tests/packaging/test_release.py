#!/usr/bin/env python3
"""Exercise release assembly using a Linux archive and a Windows package fixture."""

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile


def run(command, success=True):
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert (result.returncode == 0) == success, (command, result.stdout, result.stderr)
    return result


def linux_fixture(path, root, version, licenses, executable_mode):
    elf = bytearray(b"\x7fELF\x02\x01" + b"\0" * 14)
    struct.pack_into("<H", elf, 18, 62)
    binaries = ("bin/simxr_companion", "bin/simxr_mcp_connector", "bin/simxr_runner")
    executable = ("install.sh", "launch-desktop.sh", *binaries)
    content = {name: b"fixture" for name in (
        "include/simxr/game_bridge.h", "share/simxr/rigs/pose_sweep.luau",
        "share/doc/simxr/BUILDING.md",
        "share/simxr/sdk/python/pyproject.toml", "share/simxr/sdk/python/simxr_control/client.py",
        "share/simxr/branding/openxr-simulator.png", "share/licenses/simxr_companion/curl-headers.txt",
        *licenses, *executable)}
    for binary in (*binaries, "bin/libopenxr_simulator.so"):
        content[binary] = bytes(elf)
    content["README.txt"] = f"OpenXR Simulator {version}\n".encode()
    content["bin/openxr_simulator.json"] = json.dumps({"runtime": {"library_path": "./libopenxr_simulator.so"}}).encode()
    with tarfile.open(path, "w:gz") as archive:
        for name, data in content.items():
            info = tarfile.TarInfo(root + "/" + name)
            info.mode = executable_mode if name in executable else 0o644
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))


def main():
    linux, windows_stage, installer = map(Path, sys.argv[1:4])
    version = sys.argv[4]
    source = Path(__file__).resolve().parents[2]
    prepare = source / "packaging/prepare-release.py"
    windows_root = f"openxr-simulator-{version}-windows-x64"
    linux_root = f"openxr-simulator-{version}-linux-x64"
    with tempfile.TemporaryDirectory(prefix="simxr release ' Ω ") as temporary:
        root = Path(temporary)
        shutil.copyfile(linux, root / f"{linux_root}.tar.gz")
        shutil.copyfile(installer, root / f"{windows_root}-setup.exe")
        with zipfile.ZipFile(root / f"{windows_root}.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
            for file in sorted(windows_stage.rglob("*")):
                if file.is_file():
                    archive.write(file, windows_root + "/" + file.relative_to(windows_stage).as_posix())
        command = [sys.executable, str(prepare), "--version", version, "--directory", str(root)]
        run(command)
        lines = (root / "SHA256SUMS").read_text(encoding="ascii").splitlines()
        assert len(lines) == 4
        assert "Add application" in (root / "RELEASE_NOTES.md").read_text(encoding="utf-8")
        for line in lines:
            expected, filename = line.split("  ", 1)
            actual = hashlib.sha256()
            with (root / filename).open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    actual.update(block)
            assert expected == actual.hexdigest(), filename
        original = (root / "SHA256SUMS").read_bytes()
        failure = run(command, success=False)
        assert "Refusing to replace" in failure.stderr and (root / "SHA256SUMS").read_bytes() == original

        spec = importlib.util.spec_from_file_location("prepare_release", prepare)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        negative = root / "negative"
        negative.mkdir()
        for filename in (f"{windows_root}.zip", f"{windows_root}-setup.exe"):
            shutil.copyfile(root / filename, negative / filename)
        linux_fixture(negative / f"{linux_root}.tar.gz", linux_root, version, module.LICENSES, 0o644)
        invalid_command = [sys.executable, str(prepare), "--version", version, "--directory", str(negative)]
        failure = run(invalid_command, success=False)
        assert "non-executable" in failure.stderr
        linux_fixture(negative / f"{linux_root}.tar.gz", linux_root, version, module.LICENSES, 0o755)
        pe = bytearray(512)
        pe[:2] = b"MZ"
        struct.pack_into("<I", pe, 0x3c, 0x80)
        pe[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<H", pe, 0x84, 0x14c)
        with zipfile.ZipFile(negative / f"{windows_root}.zip", "a") as archive:
            archive.writestr(f"{windows_root}/bin/optional-32bit.dll", pe)
        failure = run(invalid_command, success=False)
        assert "optional-32bit.dll" in failure.stderr and "architecture" in failure.stderr
        assert not (negative / "SHA256SUMS").exists()
        (root / f"{linux_root}.tar.gz").unlink()
        failure = run(command, success=False)
        assert "Missing regular package" in failure.stderr
    print("PASS release assembly, hashes and refusal cases")


if __name__ == "__main__":
    main()
