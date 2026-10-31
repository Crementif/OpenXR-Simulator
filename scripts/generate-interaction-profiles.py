#!/usr/bin/env python3
"""Generate binding metadata from the pinned OpenXR registry."""
from pathlib import Path
import json
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
registry = ET.parse(root / 'include/openxr/registry/xr.xml').getroot()
extensions = {}
versions = {}
for extension in registry.findall('extensions/extension'):
    for profile in extension.findall('require/interaction_profile'):
        extensions[profile.attrib['name']] = extension.attrib['name']
for feature in registry.findall('feature'):
    if feature.get('number') not in ('1.0', '1.1'):
        continue
    for profile in feature.findall('require/interaction_profile'):
        versions.setdefault(profile.attrib['name'], feature.attrib['number'])
rows = []
for profile in registry.findall('interaction_profiles/interaction_profile'):
    name = profile.attrib['name']
    extension = extensions.get(name, '')
    version = versions.get(name)
    users = [user.attrib['path'] for user in profile.findall('user_path')]
    components = [(component, extension, version) for component in profile.findall('component')]
    for require in registry.findall("feature[@name='XR_VERSION_1_1']/require"):
        if require.get('depends') not in (None, extension):
            continue
        for addition in require.findall('extend'):
            if addition.get('interaction_profile_path') == name:
                components += [(component, '', '1.1') for component in addition.findall('component')]
    emitted = set()
    for component, requirement, core in components:
        for user in users:
            if component.get('user_path', user) != user:
                continue
            fields = [name, requirement, profile.attrib['title'], user, component.attrib['subpath']]
            core_version = 'XR_MAKE_VERSION(' + core.replace('.', ', ') + ', 0)' if core else '0'
            row = ('\t{' + ', '.join(json.dumps(field) for field in fields) + ', ' +
                   component.attrib['type'] + ', ' + component.get('system', 'false') + ', ' + core_version + '},')
            if row not in emitted:
                rows.append(row)
                emitted.add(row)
output = '// Copyright (c) 2017-2026 The Khronos Group Inc.\n'
output += '// SPDX-License-Identifier: Apache-2.0 OR MIT\n'
output += '// Generated from include/openxr/registry/xr.xml by scripts/generate-interaction-profiles.py.\n'
output += '\n'.join(rows) + '\n'
(root / 'src/simulation/InteractionProfiles.inc').write_text(output)
