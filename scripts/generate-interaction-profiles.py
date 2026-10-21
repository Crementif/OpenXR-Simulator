#!/usr/bin/env python3
"""Generate binding metadata from the pinned OpenXR registry."""
from pathlib import Path
import json
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
registry = ET.parse(root / 'include/openxr/registry/xr.xml').getroot()
extensions = {}
for extension in registry.findall('extensions/extension'):
    for profile in extension.findall('require/interaction_profile'):
        extensions[profile.attrib['name']] = extension.attrib['name']
rows = []
for profile in registry.findall('interaction_profiles/interaction_profile'):
    name = profile.attrib['name']
    users = [user.attrib['path'] for user in profile.findall('user_path')]
    if not users:
        continue
    for component in profile.findall('component'):
        for user in users:
            if component.get('user_path', user) != user:
                continue
            fields = [name, extensions.get(name, ''), profile.attrib['title'], user, component.attrib['subpath']]
            rows.append('\t{' + ', '.join(json.dumps(field) for field in fields) + ', ' +
                        component.attrib['type'] + ', ' + component.get('system', 'false') + '},')
output = '// Copyright (c) 2017-2026 The Khronos Group Inc.\n'
output += '// SPDX-License-Identifier: Apache-2.0 OR MIT\n'
output += '// Generated from include/openxr/registry/xr.xml by scripts/generate-interaction-profiles.py.\n'
output += '\n'.join(rows) + '\n'
(root / 'src/simulation/InteractionProfiles.inc').write_text(output)
