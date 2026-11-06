"""Local endpoint discovery; selection remains an explicit caller decision."""
from pathlib import Path
import os


def _data_directory(directory):
    if directory is None:
        directory = os.getenv('SIMXR_DATA_DIR')
    if directory is None:
        directory = Path(os.getenv('XDG_DATA_HOME', str(Path.home() / '.local/share'))) / 'OpenXR-Simulator'
    return Path(directory)


def endpoint(pid, directory=None):
    """The control endpoint of the OpenXR application with this process id."""
    if type(pid) is not int or pid <= 0:
        raise ValueError('A positive process id is required')
    if os.name == 'nt':
        return r'\\.\pipe\SimXR.Control.%d' % pid
    return str(_data_directory(directory) / f'control.{pid}.sock')


def discover(directory=None):
    if os.name == 'nt':
        from .windows_transport import discover as discover_pipes
        return discover_pipes()
    return sorted(path for path in _data_directory(directory).glob('control.*.sock') if path.is_socket())


