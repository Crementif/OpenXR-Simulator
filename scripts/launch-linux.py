#!/usr/bin/env python3
"""Launch an app with a child-local runtime and connect only to that child's socket."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


def existing_file(value, label):
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise ValueError(f'{label} is missing or not a file: {path}')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--companion', required=True)
    parser.add_argument('--cwd', type=Path, default=Path.cwd())
    parser.add_argument('--run-dir', type=Path, help='New, short absolute directory; defaults to /tmp/simxr-launch-*')
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--no-companion', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('Provide an application after --')
    if not 0 < args.timeout <= 300:
        parser.error('--timeout must be in (0, 300] seconds')
    try:
        manifest = existing_file(args.manifest, 'Runtime manifest')
        runtime = json.loads(manifest.read_text())['runtime']['library_path']
        existing_file(manifest.parent / runtime, 'Runtime library')
        companion = existing_file(args.companion, 'Companion')
        cwd = args.cwd.expanduser().resolve(strict=True)
        if not cwd.is_dir():
            raise ValueError(f'Working directory is not a directory: {cwd}')
        executable = (cwd / command[0]) if '/' in command[0] else shutil.which(command[0])
        if executable is None:
            raise ValueError(f'Executable not found: {command[0]}')
        command[0] = str(existing_file(executable, 'Executable'))
        for executable in (command[0], companion):
            if not os.access(executable, os.X_OK):
                raise ValueError(f'Executable permission missing: {executable}')
        if args.run_dir:
            directory = args.run_dir.expanduser().absolute()
            if len(os.fsencode(directory / 'control.2147483647.sock')) >= 108:
                raise ValueError('Run directory is too long for a Linux control socket; use /tmp')
            directory.mkdir(mode=0o700, parents=True, exist_ok=False)
        else:
            directory = Path(tempfile.mkdtemp(prefix='simxr-launch-'))
        environment = dict(os.environ, XR_RUNTIME_JSON=str(manifest), SIMXR_DATA_DIR=str(directory))
        with (directory / 'application.log').open('wb') as log:
            app = subprocess.Popen(command, cwd=cwd, env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        socket = directory / f'control.{app.pid}.sock'
        settings = directory / 'companion.settings'
        reconnect = ['env', f'SIMXR_COMPANION_SETTINGS={settings}', str(companion), '--connect', str(socket)]
        record = dict(pid=app.pid, command=command, cwd=str(cwd), manifest=str(manifest),
                      data_dir=str(directory), socket=str(socket), reconnect=reconnect)
        (directory / 'launch.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f'Run record: {directory / "launch.json"}', flush=True)
        print(f'Reopen companion: {shlex.join(reconnect)}', flush=True)
        deadline = time.monotonic() + args.timeout
        while not socket.is_socket():
            if app.poll() is not None:
                raise RuntimeError(f'Application exited with code {app.returncode}; see {directory / "application.log"}')
            if time.monotonic() >= deadline:
                raise RuntimeError(f'No runtime endpoint after {args.timeout:g}s; application PID {app.pid} '
                                   f'is left running. See {directory / "application.log"}')
            time.sleep(0.05)
        if not args.no_companion:
            with (directory / 'companion.log').open('wb') as log:
                viewer = subprocess.Popen(reconnect, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            record['companion_pid'] = viewer.pid
            (directory / 'launch.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f'Runtime endpoint ready: {socket}', flush=True)
        return 0
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print(f'Launch failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
