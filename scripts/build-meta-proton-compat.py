#!/usr/bin/env python3
"""Build local ICU aliases and a Vulkan lookup adapter for Meta XR Simulator under Proton."""

import argparse
from pathlib import Path
import re
import subprocess


def inspect(path):
    return subprocess.check_output(['llvm-objdump', '-p', str(path)], text=True)


def build(meta, proton, output):
    wine = proton.resolve().parent / 'files/lib/wine/x86_64-windows'
    imports = inspect(meta / 'MetaXRSimulator.exe')
    icu_exports = inspect(wine / 'icu.dll')
    vulkan_exports = inspect(wine / 'vulkan-1.dll')
    aliases = {}
    for name in ('icuin', 'icuuc'):
        block = imports.split(f'DLL Name: {name}.dll\n')[1].split('lookup ')[0]
        symbols = re.findall(r'^\s+\d+\s+(\w+)\s*$', block, re.MULTILINE)
        if not symbols or any(not re.search(r'\b' + symbol + r' \(forwarded', icu_exports)
                              for symbol in symbols):
            raise ValueError(f'Proton ICU cannot supply all {name} imports')
        aliases[name] = symbols
    vulkan_names = re.findall(r'^\s*\d+\s+(vk\w+) \(forwarded', vulkan_exports, re.MULTILINE)
    if 'vkGetInstanceProcAddr' not in vulkan_names or len(vulkan_names) < 100:
        raise ValueError('Unsupported Proton Vulkan export table')
    output.mkdir(parents=True, exist_ok=False)

    def link(name, entries, objects=()):
        definition = output / (name + '.def')
        definition.write_text('LIBRARY ' + name + '\nEXPORTS\n' +
                              ''.join(f'    {public}={target}\n' for public, target in entries))
        subprocess.run(['ld', '-mi386pep', '--dll', '--entry', '0', '--no-insert-timestamp',
                        '-o', str(output / (name + '.dll')), *map(str, objects), str(definition)],
                       check=True)

    for name, symbols in aliases.items():
        link(name, [(symbol, 'icu.' + symbol) for symbol in symbols])
    definition = output / 'winevulkan.def'
    definition.write_text('LIBRARY winevulkan.dll\nEXPORTS\nvkGetInstanceProcAddr\nvkGetDeviceProcAddr\n')
    library = output / 'winevulkan.lib'
    subprocess.run(['llvm-dlltool', '-m', 'i386:x86-64', '-d', str(definition), '-l', str(library)],
                   check=True)
    source = Path(__file__).resolve().parent.parent / 'tools/proton/MetaVulkanLoader.cpp'
    obj = output / 'MetaVulkanLoader.obj'
    subprocess.run(['clang', '--target=x86_64-pc-windows-msvc', '-std=c++20', '-O2',
                    '-fno-builtin', '-fno-exceptions', '-fno-rtti', '-c', str(source), '-o', str(obj)],
                   check=True)
    link('vulkan-1', [(name, 'MetaGetInstanceProcAddr' if name == 'vkGetInstanceProcAddr'
                      else 'winevulkan.' + name) for name in vulkan_names], (obj, library))
    print(f'Built {output}; no installation or runtime registration was changed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--meta-dir', type=Path, required=True)
    parser.add_argument('--proton', type=Path, required=True, help='Proton launcher file')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    args = parser.parse_args()
    try:
        build(args.meta_dir, args.proton, args.output)
    except (OSError, ValueError, IndexError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
