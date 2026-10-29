#!/usr/bin/env python3
"""
Synthetic ground-truth tests for stereo_consistency.py.

A textured ground plane and a nearer textured box are rendered for two eyes from a known
disparity field, so the correct answer is known exactly. Shadows are then added the three
ways a stereo renderer gets them wrong, and one way it gets them right.

    python test_stereo_consistency.py [--save DIR]
"""

from __future__ import annotations
import sys
from pathlib import Path

import stereo_consistency as sc

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from stereo_scene import CASES, FRUSTA, INFINITY, W, render

def main(argv) -> int:
    save = Path(argv[argv.index("--save") + 1]) if "--save" in argv else None
    if save:
        save.mkdir(parents=True, exist_ok=True)
    failures = 0
    for name, shadow, verdict, kind in CASES:
        left, right = render(shadow)
        report, sheet = sc.analyze_pair(left, right, frusta=FRUSTA)
        got_kind = report.regions[0].kind if report.regions else None
        ok = report.verdict.startswith(verdict) and (kind is None or got_kind == kind)
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {sc.describe(report)}")
        failures += 0 if ok else 1
        if save and sheet is not None:
            sheet.save(save / f"{name}.png")

    # The frusta check: the same clean pair, declared with infinity far from where it is.
    left, right = render(None)
    report, _ = sc.analyze_pair(left, right, frusta=sc.Frusta(infinity=(INFINITY + 60) / W, focal=0.45, ipd=0.064))
    ok = any("declared frusta" in note for note in report.notes)
    print(f"{'ok  ' if ok else 'FAIL'} frusta_mismatch: {report.notes[:1]}")
    failures += 0 if ok else 1

    # Identical eyes: no stereo at all.
    report, _ = sc.analyze_pair(left, left)
    ok = any("no stereo" in note for note in report.notes)
    print(f"{'ok  ' if ok else 'FAIL'} identical_eyes: {report.notes[:1]}")
    failures += 0 if ok else 1

    print("ALL PASSED" if failures == 0 else f"{failures} FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
