#!/usr/bin/env python3
"""Generate the two shared GPU history colour-conversion shaders."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--glslang', required=True, type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
header = ['#pragma once', '#include <cstdint>', 'namespace simxr::shader', '{']
provenance = {'compiler': subprocess.check_output([str(args.glslang), '--version'], text=True), 'sources': {}}
with tempfile.TemporaryDirectory() as temporary:
    for stage in ['Encode', 'Decode']:
        source = root / 'shaders' / f'History{stage}.comp'
        binary = Path(temporary) / 'shader.spv'
        subprocess.run([str(args.glslang), '-V', '--target-env', 'vulkan1.1', '-o', str(binary), str(source)], check=True)
        data = binary.read_bytes()
        words = struct.unpack(f'<{len(data)//4}I', data)
        header.append(f'inline constexpr std::uint32_t History{stage}[] = {{')
        header.extend('\t' + ', '.join(f'0x{word:08x}' for word in words[i:i+8]) + ',' for i in range(0, len(words), 8))
        header.append('};')
        provenance['sources'][source.name] = {'sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'spirv_sha256': hashlib.sha256(data).hexdigest()}
header.append('}')
(root / 'shaders/generated/HistorySpirv.h').write_text('\n'.join(header) + '\n')
(root / 'shaders/generated/History.provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
