#!/usr/bin/env python3
"""Fetch the pinned Khronos headers and build dependencies used by packaging CI."""

import argparse
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import urllib.request


HEADERS = ("https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/"
           "c46850864f4661461b0f6cb9922c058ffea4915e",
           "539ff8eaa4abf22fddbd98225c68a0c5ddb940075a4048c8b7f5d04449070a4e")
LOADER = ("https://codeload.github.com/KhronosGroup/Vulkan-Loader/tar.gz/refs/tags/vulkan-sdk-1.4.341.0",
          "fe982697c780a950641bfcf94707135c26c501352242d285fa95d087d691292e")
OPENXR = ("https://codeload.github.com/KhronosGroup/OpenXR-SDK/tar.gz/"
          "24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8",
          "492967376e28a01337874ffdef11c06e5e6aaa9d50d00efad4767f46939a48fd")
GL = "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/6af574a14089ccfee87efe230ebcdd8742859813/api/GL/"
EGL = "https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/67a3e0864c2d75ea5287b9f3d2eb74a745936692/api/KHR/"
GL_HEADERS = (
    (GL + "glext.h", "d7b6f15b1bb209e08bd6ada36983f8bb40429e66c24083aa2474d55205d97047", "GL/glext.h"),
    (GL + "wglext.h", "f6830dd670574c46dd58b119a21d857edc1dd161cba23e8644dffb6920999f68", "GL/wglext.h"),
    (EGL + "khrplatform.h", "4f1013d451392be5c3d051aea4c70b9da74ad3a1164d1e959f5f47b49a5213dd", "KHR/khrplatform.h"),
)


def download(url, expected):
    request = urllib.request.Request(url, headers={"User-Agent": "OpenXR-Simulator packaging"})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(16 * 1024 * 1024 + 1)
    if len(data) > 16 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != expected:
        raise ValueError(f"Dependency hash mismatch or download too large: {url}")
    return data


def extract(data, destination):
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        members = archive.getmembers()
        roots = set()
        for member in members:
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or "\\" in member.name or ":" in member.name or not path.parts:
                raise ValueError(f"Unsafe dependency path: {member.name}")
            if not member.isfile() and not member.isdir():
                raise ValueError(f"Unsupported dependency entry: {member.name}")
            roots.add(path.parts[0])
        if len(roots) != 1:
            raise ValueError("Dependency archive must have one root")
        for member in members:
            parts = PurePosixPath(member.name).parts[1:]
            if not parts:
                continue
            path = destination.joinpath(*parts)
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, path.open("xb") as target:
                    shutil.copyfileobj(source, target)
                os.chmod(path, member.mode & 0o777)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()  # Refuse to reuse or overwrite a previous dependency tree.
    extract(download(*HEADERS), output / "Vulkan-Headers")
    if args.platform == "windows":
        extract(download(*LOADER), output / "Vulkan-Loader")
        extract(download(*OPENXR), output / "OpenXR-SDK")
        for url, expected, name in GL_HEADERS:
            path = output / "gl" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(download(url, expected))
    print(f"Fetched pinned {args.platform} build dependencies to {output}")


if __name__ == "__main__":
    main()
