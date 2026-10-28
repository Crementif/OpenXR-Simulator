#!/usr/bin/env python3
"""Assemble versioned packages, optional Python wheels and release instructions."""

import argparse
from email.parser import BytesParser
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import tarfile
import tempfile
import zipfile


LICENSES = (
    "share/licenses/simxr/LICENSE", "share/licenses/simxr_runner/Luau.txt",
    "share/licenses/simxr_companion/SDL3.txt", "share/licenses/simxr_companion/ImGui.txt",
    "share/licenses/simxr_companion/ImGuizmo.txt", "share/licenses/simxr_companion/NanoSVG.txt",
    "share/licenses/simxr_companion/ADWAITA-SANS-LICENSE.txt",
    "share/licenses/simxr_mcp/cpp-httplib.txt", "share/licenses/simxr_mcp/nlohmann-json.txt",
    "share/licenses/simxr_mcp/stb-image-write.txt",
)


def is_pe(data, machines):
    if len(data) < 64 or data[:2] != b"MZ":
        return False
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    return offset <= len(data) - 6 and data[offset:offset + 4] == b"PE\0\0" and \
        struct.unpack_from("<H", data, offset + 4)[0] in machines


def is_elf_x64(data):
    return len(data) >= 20 and data[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", data, 18)[0] == 62


def read_prefix(stream, limit=1024 * 1024):
    return stream.read(limit)


def validate_archive(path, root, windows, version):
    common = ("README.txt", "bin/openxr_simulator.json", "include/simxr/game_bridge.h",
              "share/doc/simxr/BUILDING.md", "share/simxr/rigs/pose_sweep.luau",
              "share/simxr/sdk/python/pyproject.toml", "share/simxr/sdk/python/simxr_control/client.py", *LICENSES)
    if windows:
        binaries = ("bin/simxr_companion.exe", "bin/simxr_mcp_connector.exe", "bin/simxr_runner.exe",
                    "bin/simxr_install_check.exe")
        other = ("share/simxr/branding/openxr-simulator.ico",)
        archive = zipfile.ZipFile(path)
    else:
        binaries = ("bin/simxr_companion", "bin/simxr_mcp_connector", "bin/simxr_runner")
        other = ("install.sh", "launch-desktop.sh", "share/simxr/branding/openxr-simulator.png",
                 "share/licenses/simxr_companion/curl-headers.txt")
        archive = tarfile.open(path, "r:gz")
    with archive:
        if windows:
            entries = archive.infolist()
            if any(stat.S_ISLNK(item.external_attr >> 16) for item in entries):
                raise ValueError(f"Unexpected archive symlink in {path.name}")
            names = [item.filename for item in entries]
            if len(names) != len(set(names)):
                raise ValueError(f"Duplicate archive entries in {path.name}")
            members = {item.filename: item for item in entries if not item.is_dir()}
            def read(name):
                with archive.open(members[name]) as stream:
                    return read_prefix(stream)
        else:
            entries = archive.getmembers()
            if any(item.issym() or item.islnk() for item in entries):
                raise ValueError(f"Unexpected archive link in {path.name}")
            names = [item.name for item in entries]
            if len(names) != len(set(names)):
                raise ValueError(f"Duplicate archive entries in {path.name}")
            members = {item.name: item for item in entries if item.isfile()}
            def read(name):
                with archive.extractfile(members[name]) as stream:
                    return read_prefix(stream)
        if any(not name.startswith(root + "/") and name != root and name != root + "/" for name in names):
            raise ValueError(f"Unexpected archive root in {path.name}")
        if any(".." in Path(name).parts or "\\" in name for name in names):
            raise ValueError(f"Unsafe archive path in {path.name}")
        for relative in (*common, *binaries, *other):
            name = f"{root}/{relative}"
            if name not in members:
                raise ValueError(f"{path.name} is missing {relative}")
        readme = read(f"{root}/README.txt")
        if readme.splitlines()[:1] != [f"OpenXR Simulator {version}".encode("ascii")]:
            raise ValueError(f"{path.name} does not contain the requested version")
        manifest = json.loads(read(f"{root}/bin/openxr_simulator.json"))
        library = manifest.get("runtime", {}).get("library_path")
        libraries = ("./libopenxr_simulator.dll", "./openxr_simulator.dll") if windows else ("./libopenxr_simulator.so",)
        if library not in libraries:
            raise ValueError(f"{path.name} has an unexpected runtime manifest")
        runtime = f"bin/{library[2:]}"
        if f"{root}/{runtime}" not in members:
            raise ValueError(f"{path.name} is missing {runtime}")
        for relative in (*LICENSES, *(("share/licenses/simxr_companion/curl-headers.txt",) if not windows else ())):
            if not read(f"{root}/{relative}"):
                raise ValueError(f"{path.name} contains an empty license: {relative}")
        if windows:
            for binaries_with_license, license_directory in (
                (("bin/openxr_loader.dll", "bin/libopenxr_loader.dll"), "share/licenses/openxr/"),
                (("bin/libstdc++-6.dll", "bin/libgcc_s_seh-1.dll", "bin/libwinpthread-1.dll"),
                 "share/licenses/toolchain/"),
            ):
                needs_license = any(f"{root}/{binary}" in members for binary in binaries_with_license)
                if license_directory == "share/licenses/openxr/":
                    needs_license |= any(name.startswith(f"{root}/bin/xr_probe") and name.endswith(".exe")
                                         for name in members) or f"{root}/bin/simxr_headset_host.exe" in members
                if needs_license:
                    prefix = f"{root}/{license_directory}"
                    if not any(name.startswith(prefix) and read(name) for name in members):
                        raise ValueError(f"{path.name} is missing {license_directory}")
        for relative in (*binaries, runtime):
            data = read(f"{root}/{relative}")
            if not (is_pe(data, (0x8664,)) if windows else is_elf_x64(data)):
                raise ValueError(f"{path.name} has an unexpected binary architecture: {relative}")
        if windows:
            prefix = f"{root}/bin/"
            for name in members:
                relative = name.removeprefix(prefix)
                if name.startswith(prefix) and "/" not in relative and relative.lower().endswith((".exe", ".dll")):
                    if not is_pe(read(name), (0x8664,)):
                        raise ValueError(f"{path.name} has an unexpected binary architecture: {relative}")
        else:
            executable = ("install.sh", "launch-desktop.sh", *binaries)
            for relative in executable:
                if not members[f"{root}/{relative}"].mode & 0o111:
                    raise ValueError(f"{path.name} has a non-executable file: {relative}")


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def python_wheels(directory, version):
    if directory is None:
        return []
    wheels = []
    for name in ("simxr_control", "openxr_simulator_mcp"):
        path = directory / f"{name}-{version}-py3-none-any.whl"
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing Python distribution: {path}")
        metadata_name = f"{name}-{version}.dist-info/METADATA"
        with zipfile.ZipFile(path) as wheel:
            metadata = wheel.getinfo(metadata_name)
            if metadata.file_size > 1024 * 1024:
                raise ValueError(f"Python distribution metadata is too large: {path.name}")
            fields = BytesParser().parsebytes(wheel.read(metadata))
            if fields.get("Name", "").replace("-", "_") != name or fields.get("Version") != version:
                raise ValueError(f"Python distribution identity does not match: {path.name}")
        wheels.append(path)
    return wheels


def release_notes(version, wheels):
    prefix = f"openxr-simulator-{version}"
    notes = f"""# OpenXR Simulator {version} preview

Launch OpenXR applications, control simulated devices and inspect stereo output without a headset.

- Windows: run `{prefix}-windows-x64-setup.exe`, or extract `{prefix}-windows-x64.zip` and launch `bin/simxr_companion.exe`.
- Linux: extract `{prefix}-linux-x64.tar.gz` and run `./install.sh` or `bin/simxr_companion`.

Both platforms require a Vulkan driver and loader. Choose **Add application**, select an executable and launch it. For MCP, copy the configuration from **Preferences → MCP**. See the package README for installation and removal instructions.
"""
    if wheels:
        notes += "\nOptional Python SDK and compatibility server wheels are included.\n"
    return notes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True, help="version embedded in the packages, e.g. 2.0.0")
    parser.add_argument("--directory", type=Path, required=True, help="directory containing all three packages")
    parser.add_argument("--python-wheels", type=Path,
                        help="include the matching SDK and legacy MCP wheels from this directory")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("version must contain three numeric components")
    directory = args.directory.resolve(strict=True)
    windows_root = f"openxr-simulator-{args.version}-windows-x64"
    linux_root = f"openxr-simulator-{args.version}-linux-x64"
    windows_zip = directory / f"{windows_root}.zip"
    installer = directory / f"{windows_root}-setup.exe"
    linux_archive = directory / f"{linux_root}.tar.gz"
    files = (installer, windows_zip, linux_archive)
    wheels = python_wheels(args.python_wheels, args.version)
    for path in files:
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing regular package: {path}")
    validate_archive(windows_zip, windows_root, True, args.version)
    validate_archive(linux_archive, linux_root, False, args.version)
    with installer.open("rb") as stream:
        # Inno Setup's launcher can be PE32 even when it installs only x64 payloads.
        if not is_pe(read_prefix(stream), (0x14C, 0x8664)):
            raise ValueError("Installer is not a Windows executable")
    sums_name = "SHA256SUMS"
    notes_name = "RELEASE_NOTES.md"
    names = (notes_name, *(wheel.name for wheel in wheels), sums_name)
    for name in names:
        if (directory / name).exists() or (directory / name).is_symlink():
            raise FileExistsError(f"Refusing to replace {name}")
    with tempfile.TemporaryDirectory(prefix=".simxr-release-", dir=directory) as temporary:
        temp = Path(temporary)
        (temp / notes_name).write_text(release_notes(args.version, wheels), encoding="utf-8", newline="\n")
        for wheel in wheels:
            shutil.copyfile(wheel, temp / wheel.name)
        lines = [f"{digest(path)}  {path.name}\n" for path in files]
        lines.extend(f"{digest(temp / name)}  {name}\n" for name in names if name != sums_name)
        (temp / sums_name).write_text("".join(lines), encoding="ascii", newline="\n")
        created = []
        try:
            for name in names:
                os.link(temp / name, directory / name)
                created.append(directory / name)
        except Exception:
            for path in reversed(created):
                path.unlink()
            raise
    print(f"Prepared release instructions, {len(wheels)} Python wheels and checksums in {directory}")


if __name__ == "__main__":
    main()
