#!/usr/bin/env python3
"""Prepare a new private simulator Proton prefix, optionally opening its Windows companion."""

import argparse
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import uuid


MARKER = '.simxr-private-proton'
MARKER_LIMIT = 16384


def windows_path(path):
    text = str(path)
    if not path.is_absolute() or any(c in text for c in '\\\r\n\0<>:"|?*'):
        raise ValueError(f'Path cannot be represented through the Proton Z: drive: {path}')
    if any(part.endswith((' ', '.')) for part in path.parts[1:]):
        raise ValueError(f'Path component ends in a Windows-ambiguous character: {path}')
    return 'Z:' + text.replace('/', '\\')


def owned_directory(path, private=False):
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError(f'Directory must be owned by the current user and cannot be a symlink: {path}')
    if info.st_mode & (0o077 if private else 0o022):
        raise ValueError(f'Directory permissions are not private enough: {path}')


def read_marker(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(descriptor, 'rb') as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise ValueError(f'Ownership marker must be a private regular file owned by this user: {path}')
        data = source.read(MARKER_LIMIT + 1)
    if len(data) > MARKER_LIMIT:
        raise ValueError(f'Ownership marker exceeds {MARKER_LIMIT} bytes: {path}')
    return data.decode('utf-8')


def verify_structure(directory):
    owned_directory(directory, private=True)
    prefix = directory / 'pfx'
    owned_directory(prefix)
    owned_directory(prefix / 'drive_c')
    if (prefix / 'dosdevices/c:').resolve(strict=True) != prefix / 'drive_c':
        raise ValueError('The private prefix C: drive does not identify its own drive_c directory')
    if (prefix / 'dosdevices/z:').resolve(strict=True) != Path('/'):
        raise ValueError('The private prefix must retain the standard Z: mapping to the host filesystem')


def verify_markers(directory, proton):
    owned_directory(directory, private=True)
    try:
        metadata = read_marker(directory / MARKER)
        if metadata != read_marker(directory / 'pfx' / MARKER):
            raise ValueError('Private Proton ownership markers do not match')
    except FileNotFoundError as error:
        raise ValueError(f'Refusing to adopt an existing unowned directory: {directory}; choose a new directory') from error
    lines = metadata.split('\n')
    if (len(lines) != 7 or lines[-1] or lines[0] != 'SIMXR-PRIVATE-PROTON-1'
            or len(lines[1]) != 32 or any(c not in '0123456789abcdef' for c in lines[1])
            or lines[2] != str(os.getuid()) or lines[3] != str(directory)
            or lines[4] != windows_path(directory / 'pfx') or lines[5] != str(proton)):
        raise ValueError('Private Proton ownership metadata does not match this user, directory, or Proton launcher')
    verify_structure(directory)


def write_marker(path, metadata):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as output:
            output.write(metadata)
            output.flush()
            os.fsync(output.fileno())
    except (OSError, ValueError):
        path.unlink(missing_ok=True)
        raise


def initialize(proton, environment, log, timeout):
    process = subprocess.Popen([str(proton), 'run', 'cmd.exe', '/d', '/c', 'exit', '0'],
                               env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        result = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired as reap_error:
            raise RuntimeError(f'Initialization timed out and its terminated launcher PID {process.pid} '
                               'could not be reaped within 5 seconds') from reap_error
        raise RuntimeError(f'Proton initialization exceeded {timeout:g} seconds; its process group was terminated') from error
    if result:
        raise RuntimeError(f'Proton initialization exited with code {result}')


def provision(directory, proton, environment, timeout):
    if directory.exists() or directory.is_symlink():
        verify_markers(directory, proton)
        print(f'Reusing verified private directory: {directory}', flush=True)
        return

    directory.mkdir(mode=0o700)
    marked = []
    try:
        with (directory / 'provision.log').open('xb') as log:
            initialize(proton, environment, log, timeout)
        verify_structure(directory)
        metadata = '\n'.join(('SIMXR-PRIVATE-PROTON-1', uuid.uuid4().hex, str(os.getuid()),
                              str(directory), windows_path(directory / 'pfx'), str(proton), ''))
        if len(metadata.encode('utf-8')) > MARKER_LIMIT:
            raise ValueError('Private Proton ownership metadata exceeds its size limit')
        for marker in (directory / 'pfx' / MARKER, directory / MARKER):
            write_marker(marker, metadata)
            marked.append(marker)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        for marker in marked:
            marker.unlink(missing_ok=True)
        raise RuntimeError(f'{error}. Initialization left an unmarked directory at {directory}; '
                           f'see {directory / "provision.log"}. Choose a new directory for another attempt.') from error


def existing_path(value, directory=False):
    path = Path(value).expanduser().resolve(strict=True)
    if not (path.is_dir() if directory else path.is_file()):
        raise ValueError(f'Expected an existing {"directory" if directory else "file"}: {path}')
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compat-data', required=True,
                        help='Explicit new absolute compatibility-data directory; existing unowned directories are refused')
    parser.add_argument('--proton', required=True, help='Standard Proton launcher script')
    parser.add_argument('--steam-client', required=True, help='Steam client installation directory')
    parser.add_argument('--companion', help='Windows companion executable to launch inside the selected prefix')
    parser.add_argument('--timeout', type=float, default=120, help='Initialization timeout in seconds (default: 120)')
    parser.add_argument('arguments', nargs=argparse.REMAINDER, help='Optional companion arguments after --')
    args = parser.parse_args()
    if not 0 < args.timeout <= 600:
        parser.error('--timeout must be in (0, 600] seconds')
    if args.arguments and not args.companion:
        parser.error('Companion arguments require --companion')
    try:
        if not sys.platform.startswith('linux'):
            raise ValueError('Private Proton provisioning must run on Linux')
        proton = existing_path(args.proton)
        if proton.name != 'proton' or not os.access(proton, os.X_OK) or not (proton.parent / 'files').is_dir():
            raise ValueError('--proton must identify an executable standard Proton launcher with its files directory')
        steam = existing_path(args.steam_client, directory=True)
        selected = Path(args.compat_data).expanduser()
        if not selected.is_absolute() or not selected.name or selected.name in ('.', '..'):
            raise ValueError('--compat-data must name an explicit new absolute directory')
        directory = selected.parent.resolve(strict=True) / selected.name
        windows_path(directory / 'pfx')
        windows_path(proton)
        for protected in (Path.home() / '.wine', proton.parent, steam / 'steamapps/compatdata'):
            if directory.is_relative_to(protected.resolve()):
                raise ValueError(f'Choose a dedicated simulator directory outside shared/default prefixes: {protected}')
        for parent in directory.parents:
            if (parent / 'system.reg').exists() or (parent / 'pfx/system.reg').exists():
                raise ValueError(f'Choose a directory outside existing Wine/Proton prefixes: {parent}')
        companion = existing_path(args.companion) if args.companion else None
        if companion:
            windows_path(companion)
            with companion.open('rb') as source:
                if source.read(2) != b'MZ':
                    raise ValueError('--companion must identify a Windows executable')
        environment = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(directory),
                           STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam))
        for name in ('WINEPREFIX', 'XR_RUNTIME_JSON', 'SIMXR_CONTROL_SOCKET'):
            environment.pop(name, None)
        provision(directory, proton, environment, args.timeout)
        print(f'Private Proton prefix for the launch profile: {windows_path(directory / "pfx")}', flush=True)
        print('Runtime registration is performed only when launching a profile that explicitly selects this prefix.',
              flush=True)
        if companion:
            data = directory / 'runtime-data'
            data.mkdir(mode=0o700, exist_ok=True)
            owned_directory(data, private=True)
            environment['SIMXR_DATA_DIR'] = windows_path(data)
            environment['SIMXR_COMPANION_SETTINGS'] = windows_path(directory / 'companion.settings')
            arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
            with (directory / 'companion.log').open('ab') as log:
                process = subprocess.Popen([str(proton), 'run', windows_path(companion), *arguments],
                                           env=environment, stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
            print(f'Companion started (launcher PID {process.pid}); log: {directory / "companion.log"}', flush=True)
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Private Proton setup failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
