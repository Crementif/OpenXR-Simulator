"""Validate ELF requirements for the updated Ubuntu 24.04 x64 baseline."""

import os
from pathlib import Path
import re
import struct
import subprocess


MAX_VERSIONS = {
    'GLIBC': (2, 39),
    'GLIBCXX': (3, 4, 33),
    'CXXABI': (1, 3, 15),
    'GCC': (14, 0, 0),
}
SPECIAL_VERSIONS = {'GLIBC_ABI_DT_RELR', 'CXXABI_FLOAT128', 'CXXABI_TM_1', 'GLIBCXX_DEBUG_MESSAGE_LENGTH'}
SYSTEM_LIBRARIES = {
    'libc.so.6', 'libm.so.6', 'libstdc++.so.6', 'libgcc_s.so.1',
    'libdl.so.2', 'libpthread.so.0', 'librt.so.1', 'ld-linux-x86-64.so.2',
    'libvulkan.so.1', 'libopenxr_loader.so.1',
}


def check_version_requirements(output, label, require_needs=False):
    marker = 'Version needs section '
    if marker not in output:
        if require_needs:
            raise ValueError(f'{label} has dynamic version requirements that readelf could not inspect')
        return
    needs = output.split(marker, 1)[1]
    needs = re.split(r'\nVersion (?:symbols|definition|needs) section ', needs, maxsplit=1)[0]
    for name in re.findall(r'\bName:\s+(\S+)', needs):
        if name in SPECIAL_VERSIONS:
            continue
        match = re.fullmatch(r'(GLIBC|GLIBCXX|CXXABI|GCC)_([0-9]+(?:\.[0-9]+)*)', name)
        if not match:
            raise ValueError(f'{label} requires unsupported symbol version {name}')
        version = tuple(int(part) for part in match.group(2).split('.'))
        limit = MAX_VERSIONS[match.group(1)]
        width = max(len(version), len(limit))
        if version + (0,) * (width - len(version)) > limit + (0,) * (width - len(limit)):
            raise ValueError(f'{label} requires {name}, newer than the updated Ubuntu 24.04 baseline')


def check_dynamic_requirements(output, label):
    needed = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', output))
    unsupported = sorted(needed - SYSTEM_LIBRARIES)
    if unsupported:
        raise ValueError(f'{label} requires unqualified shared libraries: {", ".join(unsupported)}')
    for search in re.findall(r'\((?:RPATH|RUNPATH)\).*?\[([^\]]*)\]', output):
        if any(entry not in ('$ORIGIN', '${ORIGIN}') for entry in search.split(':')):
            raise ValueError(f'{label} contains an unsupported runtime library search path: {search}')


def check_elf(path):
    path = Path(path)
    with path.open('rb') as stream:
        header = stream.read(64)
        size = os.fstat(stream.fileno()).st_size
    if len(header) < 64 or header[:6] != b'\x7fELF\x02\x01' or header[18:20] != b'\x3e\x00':
        raise ValueError(f'Expected a Linux x64 ELF binary: {path}')
    section_offset = struct.unpack_from('<Q', header, 40)[0]
    section_size, section_count, section_names = struct.unpack_from('<HHH', header, 58)
    if (not section_offset or section_size != 64 or not section_count or not section_names or
            section_names >= section_count or section_offset > size or
            section_count * section_size > size - section_offset):
        raise ValueError(f'{path} has no inspectable ELF section table')
    environment = dict(os.environ, LC_ALL='C')
    try:
        versions = subprocess.run(['readelf', '--wide', '--version-info', str(path)], env=environment,
                                  capture_output=True, text=True, check=True, timeout=30)
        dynamic = subprocess.run(['readelf', '--wide', '--dynamic', str(path)], env=environment,
                                 capture_output=True, text=True, check=True, timeout=30)
    except FileNotFoundError as error:
        raise ValueError('Linux package validation requires readelf from GNU binutils') from error
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise ValueError(f'Cannot inspect ELF requirements for {path}') from error
    has_needs = bool(re.search(r'\((?:VERNEED|VERNEEDNUM)\)', dynamic.stdout))
    check_version_requirements(versions.stdout, str(path), has_needs)
    check_dynamic_requirements(dynamic.stdout, str(path))
