#!/usr/bin/env python3
"""Generate checked-in compositor SPIR-V from the one canonical HLSL source.

DXC is required. Optional SPIRV-Cross produces desktop GLSL and reflection JSON.
All generated metadata is deterministic: no timestamps or machine-local paths.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(args):
    try:
        return subprocess.run([str(arg) for arg in args], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
    except subprocess.CalledProcessError as error:
        sys.stderr.write(error.stderr.decode(errors='replace'))
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dxc', required=True, type=Path)
    parser.add_argument('--spirv-cross', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'shaders/generated')
    parser.add_argument('--check', action='store_true', help='Fail if checked-in files differ; never modifies output')
    args = parser.parse_args()
    source = ROOT / 'shaders/Compositor.hlsl'
    metadata = {
        'source': 'shaders/Compositor.hlsl',
        'source_sha256': digest(source.read_bytes()),
        'generator_sha256': digest(Path(__file__).read_bytes()),
        'dxc': {'version': run([args.dxc, '--version']).decode().strip(), 'sha256': digest(args.dxc.read_bytes())},
        'targets': {},
    }
    libraries = {}
    for name in ['libdxcompiler.so', 'libdxil.so']:
        library = args.dxc.resolve().parent.parent / 'lib' / name
        if library.exists():
            libraries[name] = digest(library.read_bytes())
    if libraries:
        metadata['dxc']['libraries'] = libraries
    generated = {}
    arrays = []
    with tempfile.TemporaryDirectory(prefix='simxr-compositor-') as temporary:
        for name, entry, profile in [('vertex', 'VSMain', 'vs_6_0'), ('pixel', 'PSMain', 'ps_6_0'), ('depth_ms', 'PSMain', 'ps_6_0')]:
            flags = ['-spirv', '-fspv-target-env=vulkan1.1', '-T', profile, '-E', entry, '-D', 'SIMXR_VULKAN', '-O3', '-Ges', '-WX']
            if name == 'depth_ms':
                flags += ['-D', 'SIMXR_DEPTH_MS']
            if name == 'vertex':
                flags += ['-fvk-invert-y']
            binary = Path(temporary) / f'{name}.spv'
            run([args.dxc, *flags, source, '-Fo', binary])
            data = binary.read_bytes()
            if len(data) % 4 or struct.unpack_from('<I', data)[0] != 0x07230203:
                raise RuntimeError('DXC did not produce valid SPIR-V words')
            words = struct.unpack(f'<{len(data)//4}I', data)
            rows = [', '.join(f'0x{word:08x}u' for word in words[i:i+8]) for i in range(0, len(words), 8)]
            arrays += [f'inline constexpr std::array<std::uint32_t, {len(words)}> {name} = {{\n\t' + ',\n\t'.join(rows) + '\n};']
            generated[f'Compositor.{name}.spv'] = data
            metadata['targets'][name] = {'entry': entry, 'flags': flags, 'sha256': digest(data)}
            if args.spirv_cross:
                cross_flags = ['--version', '450', '--no-es', '--combined-samplers-inherit-bindings']
                if name == 'vertex':
                    cross_flags += ['--flip-vert-y', '--fixup-clipspace']
                glsl = run([args.spirv_cross, binary, *cross_flags])
                metadata['targets'][name]['glsl_flags'] = cross_flags
                generated[f'Compositor.{name}.glsl'] = glsl
                reflection = json.loads(run([args.spirv_cross, binary, '--reflect']))
                if name in ('pixel', 'depth_ms'):
                    constants = reflection.get('push_constants', [])
                    if len(constants) != 1:
                        raise RuntimeError('Pixel shader must contain one push-constant block')
                    members = reflection['types'][constants[0]['type']]['members']
                    expected = ['eyeTangents', 'sourceTangents', 'sourceFromEyeRotation', 'sourceFromEyePosition', 'imageRectangle', 'quadParameters', 'control']
                    if [(member['name'], member['offset']) for member in members] != list(zip(expected, range(0, 112, 16))):
                        raise RuntimeError('Shader constants do not match seven-slot C++ layout')
                    if [image['binding'] for image in reflection.get('separate_images', [])] != [0] or [sampler['binding'] for sampler in reflection.get('separate_samplers', [])] != ([] if name == 'depth_ms' else [1]):
                        raise RuntimeError('Shader texture/sampler descriptor bindings changed')
                generated[f'Compositor.{name}.reflection.json'] = (json.dumps(reflection, indent=2, sort_keys=True) + '\n').encode()
        if args.spirv_cross:
            metadata['spirv_cross'] = {'sha256': digest(args.spirv_cross.read_bytes())}
    header = '// Generated by scripts/generate-compositor-shaders.py; do not edit.\n'
    header += f'// HLSL SHA-256: {metadata["source_sha256"]}\n#pragma once\n\n#include <array>\n#include <cstdint>\n\nnamespace simxr::shaders\n{{\n'
    header += '\n\n'.join(arrays) + '\n}\n'
    generated['CompositorSpirv.h'] = header.encode()
    generated['Compositor.provenance.json'] = (json.dumps(metadata, indent=2, sort_keys=True) + '\n').encode()
    optional_names = [f'Compositor.{stage}.{kind}' for stage in ['vertex', 'pixel', 'depth_ms'] for kind in ['glsl', 'reflection.json']]
    stale = [name for name in optional_names if name not in generated and (args.output / name).exists()]
    if args.check:
        changed = [name for name, data in generated.items() if not (args.output / name).exists() or (args.output / name).read_bytes() != data]
        changed += stale
        if changed:
            raise SystemExit('Generated compositor assets differ: ' + ', '.join(changed))
        print('Compositor generated assets match canonical source and tools')
        return
    args.output.mkdir(parents=True, exist_ok=True)
    for name in stale:
        (args.output / name).unlink()
    for name, data in generated.items():
        (args.output / name).write_bytes(data)
    print(f'Generated {len(generated)} compositor assets')


if __name__ == '__main__':
    main()
