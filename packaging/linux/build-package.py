#!/usr/bin/env python3
"""Build a relocatable Linux archive from one CMake installation stage."""

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tarfile
import tempfile

from abi import check_elf


def configured_version(build):
    header = (build / "generated/simxr/Version.h").read_text(encoding="utf-8")
    match = re.search(r'Version\[\]\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"', header)
    if not match:
        raise ValueError("Could not read the configured simulator version")
    return match.group(1)


def validate_stage(stage):
    files = [
        "README.txt", "install.sh", "launch-desktop.sh", "bin/openxr_simulator.json",
        "bin/simxr_companion", "bin/simxr_mcp_connector", "bin/simxr_runner",
        "bin/libopenxr_simulator.so", "bin/fonts/AdwaitaSans-Regular.ttf",
        "share/simxr/branding/openxr-simulator.png", "share/simxr/rigs/pose_sweep.luau",
        "include/simxr/game_bridge.h", "share/doc/simxr/BUILDING.md",
        "share/simxr/sdk/python/pyproject.toml", "share/simxr/sdk/python/simxr_control/client.py",
        "share/licenses/simxr/LICENSE", "share/licenses/simxr_runner/Luau.txt",
        "share/licenses/simxr_companion/SDL3.txt", "share/licenses/simxr_companion/ImGui.txt",
        "share/licenses/simxr_companion/ImGuizmo.txt", "share/licenses/simxr_companion/NanoSVG.txt",
        "share/licenses/simxr_companion/ADWAITA-SANS-LICENSE.txt",
        "share/licenses/simxr_companion/curl-headers.txt",
        "share/licenses/simxr_mcp/cpp-httplib.txt", "share/licenses/simxr_mcp/nlohmann-json.txt",
        "share/licenses/simxr_mcp/stb-image-write.txt",
    ]
    for relative in files:
        if not (stage / relative).is_file():
            raise ValueError(f"The staged installation is incomplete: {relative}")
        if relative.startswith("share/licenses/") and (stage / relative).stat().st_size == 0:
            raise ValueError(f"The staged license is empty: {relative}")
    manifest = json.loads((stage / "bin/openxr_simulator.json").read_text(encoding="utf-8"))
    if manifest.get("runtime", {}).get("library_path") != "./libopenxr_simulator.so":
        raise ValueError("The runtime manifest must point to its adjacent shared library")
    for name in ("simxr_companion", "simxr_mcp_connector", "simxr_runner", "libopenxr_simulator.so"):
        path = stage / "bin" / name
        with path.open("rb") as stream:
            header = stream.read(20)
        if len(header) != 20 or header[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", header, 18)[0] != 62:
            raise ValueError(f"Expected a Linux x64 ELF binary: {path}")
    for name in ("install.sh", "launch-desktop.sh", "bin/simxr_companion", "bin/simxr_mcp_connector", "bin/simxr_runner"):
        if not os.access(stage / name, os.X_OK):
            raise ValueError(f"The staged file is not executable: {name}")
    for path in stage.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"The stage contains an unexpected symlink: {path.relative_to(stage)}")
        if path.is_file():
            with path.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    check_elf(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True, help="configured, built CMake directory")
    parser.add_argument("--output", type=Path, required=True, help="directory for the archive")
    parser.add_argument("--configuration", default="Release")
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    version = configured_version(build)
    name = f"openxr-simulator-{version}-linux-x64"
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    destination = output / f"{name}.tar.gz"
    if destination.exists():
        raise FileExistsError(f"Refusing to replace an existing package: {destination}")
    with tempfile.TemporaryDirectory(prefix=".simxr-build-", dir=output) as temporary:
        stage = Path(temporary) / name
        subprocess.run(["cmake", "--install", str(build), "--config", args.configuration,
                        "--prefix", str(stage)], check=True)
        validate_stage(stage)
        archive = Path(temporary) / destination.name
        with tarfile.open(archive, "w:gz", compresslevel=6, format=tarfile.PAX_FORMAT) as output_archive:
            output_archive.add(stage, arcname=name)
        # Link within the output filesystem to make refusal atomic if another builder wins.
        os.link(archive, destination)
    print(f"Created {destination}")


if __name__ == "__main__":
    main()
