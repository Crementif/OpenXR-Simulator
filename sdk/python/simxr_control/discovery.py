"""Local endpoint discovery; selection remains an explicit caller decision."""
from pathlib import Path
import os


def discover(directory=None):
    if os.name == 'nt':
        from .windows_transport import discover as discover_pipes
        return discover_pipes()
    if directory is None:
        directory = os.getenv('SIMXR_DATA_DIR')
    if directory is None:
        directory = Path(os.getenv('XDG_DATA_HOME', str(Path.home() / '.local/share'))) / 'OpenXR-Simulator'
    return sorted(path for path in Path(directory).glob('control.*.sock') if path.is_socket())


