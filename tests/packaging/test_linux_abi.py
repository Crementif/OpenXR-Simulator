#!/usr/bin/env python3
"""Check release ABI gates against representative ELF requirement tables."""
import sys
import tempfile
import shutil
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'packaging' / 'linux'))
from abi import check_dynamic_requirements, check_elf, check_version_requirements


def reject(function, value, text):
    try:
        function(value, 'fixture')
    except ValueError as error:
        assert text in str(error), error
    else:
        raise AssertionError(f'Accepted incompatible requirements: {value}')


def main():
    current = '''Version needs section '.gnu.version_r' contains 3 entries:
  Name: GLIBC_2.39  Flags: none
  Name: GLIBCXX_3.4.33  Flags: none
  Name: CXXABI_1.3.15  Flags: none
  Name: GCC_14.0.0  Flags: none
  Name: GLIBC_ABI_DT_RELR  Flags: none
'''
    check_version_requirements(current + "\nVersion definition section '.gnu.version_d' contains 1 entry:\n  Name: GLIBC_2.99\n", 'fixture')
    reject(check_version_requirements, current + ' Name: GLIBC_2.40\n', 'GLIBC_2.40')
    reject(check_version_requirements, current + ' Name: GLIBCXX_3.4.34\n', 'GLIBCXX_3.4.34')
    reject(check_version_requirements, current + ' Name: GLIBC_PRIVATE\n', 'GLIBC_PRIVATE')
    try:
        check_version_requirements('No version information found in this file.', 'fixture', True)
    except ValueError as error:
        assert 'could not inspect' in str(error), error
    else:
        raise AssertionError('Accepted inaccessible dynamic version requirements')
    dynamic = '''0x0001 (NEEDED) Shared library: [libc.so.6]
0x0001 (NEEDED) Shared library: [libvulkan.so.1]
0x001d (RUNPATH) Library runpath: [$ORIGIN]
'''
    check_dynamic_requirements(dynamic, 'fixture')
    reject(check_dynamic_requirements, dynamic + '0x0001 (NEEDED) Shared library: [libnew.so.1]\n', 'libnew.so.1')
    reject(check_dynamic_requirements, dynamic + '0x001d (RUNPATH) Library runpath: [/tmp/build]\n', '/tmp/build')
    if len(sys.argv) > 1:
        checked = 0
        for file in Path(sys.argv[1]).rglob('*'):
            if file.is_file():
                with file.open('rb') as stream:
                    is_elf = stream.read(4) == b'\x7fELF'
                if is_elf:
                    check_elf(file)
                    checked += 1
        assert checked, 'The supplied stage contained no ELF files'
        source = Path(sys.argv[1]) / 'bin' / 'simxr_mcp_connector'
        with tempfile.TemporaryDirectory(prefix='simxr-sectionless-') as temporary:
            broken = Path(temporary) / 'connector'
            shutil.copyfile(source, broken)
            with broken.open('r+b') as stream:
                stream.seek(40)
                stream.write(b'\0' * 8)
                stream.seek(58)
                stream.write(b'\0' * 6)
            try:
                check_elf(broken)
            except ValueError as error:
                assert 'no inspectable ELF section table' in str(error), error
            else:
                raise AssertionError('Accepted an ELF with its section table removed')
    print('PASS Linux x64 baseline requirement checks')


if __name__ == '__main__':
    main()
