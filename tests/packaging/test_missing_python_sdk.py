#!/usr/bin/env python3
"""A missing SDK must not silently select file-based compatibility control."""

from pathlib import Path
import sys
import tempfile
from unittest import mock

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'mcp-server'))
import debug_control
import legacy_control


def missing_sdk(name, *args, **kwargs):
    if name == 'simxr_control':
        raise ModuleNotFoundError("No module named 'simxr_control'", name='simxr_control')
    raise AssertionError(f'Unexpected dynamic import: {name}')


def main():
    with tempfile.TemporaryDirectory(prefix='simxr-no-sdk-') as directory:
        legacy = legacy_control.LegacyControl(directory)
        debug = debug_control.DebugTools()
        try:
            with mock.patch('importlib.import_module', side_effect=missing_sdk):
                result = debug.call('debug_connect', {})
                assert result['error'] == 'unavailable' and 'simxr_control' in result['detail'], result
                result = legacy.call('set_head_pose', {})
                assert result is not None and not result['ok'] and 'simxr_control' in result['error'], result
        finally:
            debug.close()
            legacy.close()
    print('PASS missing SDK reports an error instead of selecting legacy files')


if __name__ == '__main__':
    main()
