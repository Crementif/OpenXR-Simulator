#!/usr/bin/env python3
"""Check portable MCP configuration output without starting a simulator."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

try:
    import tomllib
except ImportError:
    tomllib = None


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='simxr-config-') as temporary:
        root = Path(temporary)
        binary = root / 'portable Ω $ quote' / 'bin' / executable.name
        binary.parent.mkdir(parents=True)
        shutil.copy2(executable, binary)
        app = root / 'development ü " app' / ('simxr_companion.exe' if os.name == 'nt' else 'simxr_companion')
        if os.name == 'nt':
            app = root / 'development ü app' / 'simxr_companion.exe'
        env = dict(os.environ, SIMXR_MCP_DISCOVERY_DIR=str(root / 'must-not-exist'), PATH='')
        env.pop('SIMXR_MCP_TOKEN', None)

        def run(*args):
            return subprocess.run([str(binary), *args], env=env, cwd=root, capture_output=True, timeout=15)

        for flags, expected in (((), []), (('--windowless',), ['--windowless']),
                                (('--app', str(app), '--windowless'), ['--app', str(app), '--windowless'])):
            result = run('--print-config', *flags)
            assert result.returncode == 0 and not result.stderr, (result.returncode, result.stderr)
            config = json.loads(result.stdout.decode('utf-8'))
            server = config['mcpServers']['openxr-simulator']
            assert Path(server['command']).resolve() == binary.resolve() and server['args'] == expected, server
            assert not (root / 'must-not-exist').exists()
        codex_app = app if os.name == 'nt' else app / 'control \n \t \x7f path'
        for flags, expected in (((), []), (('--windowless',), ['--windowless']),
                                (('--app', str(codex_app), '--windowless'), ['--app', str(codex_app), '--windowless'])):
            result = run('--print-config', '--client', 'codex', *flags)
            assert result.returncode == 0 and not result.stderr, (result.returncode, result.stderr)
            text = result.stdout.decode('utf-8')
            if tomllib is not None:
                config = tomllib.loads(text)
                assert set(config) == {'mcp_servers'}, config
                server = config['mcp_servers']['openxr-simulator']
            else:
                # Generated values use the JSON subset of TOML; keep Python 3.10 support.
                lines = text.splitlines()
                assert lines[0] == '[mcp_servers.openxr-simulator]'
                server = {key.strip(): json.loads(value) for key, value in
                          (line.split('=', 1) for line in lines[1:])}
            assert Path(server['command']).resolve() == binary.resolve() and server['args'] == expected, server
            assert not (root / 'must-not-exist').exists()
        assert run('--print-config', '--client', 'generic').stdout == run('--print-config').stdout
        for args in (('--client', 'codex'), ('--print-config', '--client'),
                     ('--print-config', '--client', 'unknown'),
                     ('--print-config', '--client', 'codex', '--client', 'codex'),
                     ('--print-config', '--app', 'relative'), ('--print-config', '--windowless', '--windowless'),
                     ('--print-config', '--port', '1234'), ('--print-config', '--print-config')):
            failed = run(*args)
            assert failed.returncode != 0 and not failed.stdout, (args, failed.stdout, failed.stderr)
            assert not (root / 'must-not-exist').exists()
        print('PASS portable JSON/TOML MCP configuration, escaped paths, options and no startup')


if __name__ == '__main__':
    main()
