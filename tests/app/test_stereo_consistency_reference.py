#!/usr/bin/env python3
"""Compare native consistency reports against the existing synthetic Python scenes."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image

source = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(source / 'mcp-server'))
import stereo_consistency as reference
sys.path.insert(0, str(source / "tests"))
import stereo_scene as fixture


def run(executable, root, name, left, right, expected, kind='', mode=''):
    pair = root / (name + '.rgb')
    sheet = root / (name + '.rgba')
    with pair.open('wb') as output:
        output.write(left.resize((480, 300), Image.Resampling.BILINEAR).tobytes())
        output.write(right.resize((480, 300), Image.Resampling.BILINEAR).tobytes())
    command = [executable, str(pair), expected, kind, str(sheet)]
    if mode:
        command.append(mode)
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (name, result.stdout, result.stderr)
    return json.loads(result.stdout), sheet


def main():
    executable = str(Path(sys.argv[1]).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix='simxr-stereo-reference-') as temporary:
        root = Path(temporary)
        for name, shadow, verdict, kind in fixture.CASES:
            left, right = fixture.render(shadow)
            expected, _ = reference.analyze_pair(left, right, frusta=fixture.FRUSTA, want_images=False)
            actual, sheet = run(executable, root, name, left, right, verdict, kind or '')
            assert actual['verdict'] == expected.verdict, (name, actual, expected)
            for field in ('score', 'highlight_score', 'valid_pct', 'median_disparity_pct', 'far_disparity_pct'):
                assert abs(actual[field] - getattr(expected, field)) < .5, (name, field, actual[field], getattr(expected, field))
            if kind:
                assert actual['regions'][0]['nature'] == expected.regions[0].nature, (name, actual, expected)
            with Image.frombytes('RGBA', (964, 624), sheet.read_bytes()) as image:
                assert image.getbbox()[2:] == (964, 624), name
        left, right = fixture.render(None)
        mismatch, _ = run(executable, root, 'mismatch', left, right, 'PASS', mode='mismatch')
        assert any('frusta' in note for note in mismatch['notes']), mismatch
        identical, _ = run(executable, root, 'identical', left, left, 'PASS', mode='identical')
        assert any('no stereo' in note for note in identical['notes']), identical
    print('PASS native stereo consistency against all synthetic Python reference scenes')


if __name__ == '__main__':
    main()
