#!/usr/bin/env python3
"""Check that relocated package links reach the desktop opener as file URLs."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    fixture = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='simxr-doc-links-') as temporary:
        root = Path(temporary) / 'moved Ω # package'
        binary = root / 'bin'
        binary.mkdir(parents=True)
        executable = binary / 'package_documentation_tests'
        shutil.copy2(fixture, executable)
        guide = root / 'README.txt'
        guide.write_text('test guide', encoding='utf-8')
        licenses = root / 'share' / 'licenses'
        licenses.mkdir(parents=True)
        opener = Path(temporary) / 'opener'
        opener.mkdir()
        script = opener / 'xdg-open'
        script.write_text('#!/bin/sh\nprintf "%s" "$1" > "$SIMXR_TEST_URI_CAPTURE"\n', encoding='utf-8')
        script.chmod(0o755)
        marker = Path(temporary) / 'uri.txt'
        env = dict(os.environ, PATH=str(opener), DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-simxr-test-bus',
                   SIMXR_TEST_URI_CAPTURE=str(marker))
        for option, expected in [('--open-guide', guide), ('--open-licenses', licenses)]:
            marker.unlink(missing_ok=True)
            subprocess.run([str(executable), option], env=env, check=True, timeout=10)
            deadline = time.monotonic() + 10
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            assert marker.exists() and marker.read_text(encoding='ascii') == expected.as_uri(), (option, expected)
        guide.unlink()
        assert subprocess.run([str(executable), '--open-guide'], env=env, timeout=10).returncode != 0
    print('PASS relocated documentation links and missing guide')


if __name__ == '__main__':
    main()
