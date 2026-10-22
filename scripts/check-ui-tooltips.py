#!/usr/bin/env python3
"""Grep-based companion icon tooltip audit (run from the repository root)."""
from pathlib import Path
import re
import subprocess

files = subprocess.check_output(["rg", "--files", "src/app", "-g", "*.cpp"], text=True).splitlines()
failures = []
icons = 0
# Custom input surfaces are instruments, not icon buttons. Their interaction help
# is checked in the F10 sampled review. All other InvisibleButtons need local help.
instruments = {
    "Controls.cpp": {"id", '"##Chip"'},
    "InputPad.cpp": {"id"},
    "ReachMap.cpp": {"id"},
    "RotationOrb.cpp": {'"Orb"'},
    "RigView.cpp": {'"Orbit"'},
    "TimelineCanvas.cpp": {'"Timeline canvas"'},
    "WorkspacePanels.cpp": {"id"},
}
for filename in files:
    source = Path(filename).read_text()
    icons += len(re.findall(r'\bIconButton\(\s*"', source))
    for match in re.finditer(r'\bIconButton\(\s*"[^"\n]*"\s*,[^,]+,\s*(nullptr|""\s*[,\)])', source):
        failures.append((filename, source.count("\n", 0, match.start()) + 1, "empty icon tooltip"))
    for match in re.finditer(r'ImGui::(SmallButton|Button|InvisibleButton)\(\s*("[^"\n]*"|id|label)', source):
        kind, label = match.groups()
        if kind == "InvisibleButton":
            if label in instruments.get(Path(filename).name, set()):
                continue
            # The rail has a status/shortcut tooltip in its shared implementation.
            if Path(filename).name == "Controls.cpp" and label == "label":
                continue
        elif label.strip('"').split("##")[0] not in {"?", "x", "+", "-", "<", ">", "^", "v", ""}:
            continue
        following = source[match.end():].splitlines()[:32]
        if not re.search(r'(SetItemTooltip|SetTooltip|BeginTooltip)\(', "\n".join(following)):
            failures.append((filename, source.count("\n", 0, match.start()) + 1, "icon button needs a tooltip"))
for filename, line, issue in failures:
    print(f"{filename}:{line}: {issue}")
print(f"{icons} IconButton calls; {len(failures)} missing tooltips")
raise SystemExit(bool(failures))
