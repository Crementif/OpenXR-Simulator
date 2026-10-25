"""Explicit MP4 download with bounded reads and atomic file publication."""
import math
import os
from pathlib import Path
import tempfile
import time

from .errors import ControlTimeout
from .protocol import MAX_PAYLOAD, ProtocolError


def save_video(client, session, first_frame, last_frame, path, *, timeout=120.0, overwrite=False):
    destination = Path(path).absolute()
    if destination.suffix.lower() != '.mp4':
        raise ValueError('Use an .mp4 destination')
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('A positive finite timeout is required')
    if not overwrite and destination.exists():
        raise FileExistsError(destination)
    deadline = time.monotonic() + timeout
    ticket = None
    temporary = None
    disconnected = False

    def exchange(call):
        nonlocal disconnected
        while True:
            try:
                return call()
            except ControlTimeout as error:
                if time.monotonic() >= deadline:
                    # Closing retires even a ticket whose admission reply was lost.
                    client.close()
                    disconnected = True
                    raise ConnectionError('Video transfer timed out. The connection was closed to release its resources.') from error
                call = client.retry

    try:
        ticket = exchange(lambda: client.start_video_export(session, first_frame, last_frame))['export_id']
        while True:
            info = exchange(lambda: client.export_info(ticket))
            if info['phase'] == 'complete':
                break
            if info['phase'] == 'failed':
                raise RuntimeError(info.get('detail') or 'Video export failed')
            if info['phase'] != 'pending':
                raise ProtocolError('Unknown video export phase')
            if time.monotonic() >= deadline:
                raise TimeoutError('Video export deadline expired')
            time.sleep(0.02)
        size = info['total_bytes']
        if info.get('media_type') != 'video/mp4' or not 0 < size <= 512 * 1024 * 1024:
            raise ProtocolError('Invalid MP4 export description')
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + '.', delete=False) as output:
            temporary = Path(output.name)
            offset = 0
            while offset < size:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Video download deadline expired')
                chunk = exchange(lambda: client.read_export(ticket, offset, min(MAX_PAYLOAD - 4096, size - offset)))
                data = chunk['bytes']
                if (chunk['offset'] != offset or chunk['length'] != len(data) or not data or
                        len(data) > size - offset or chunk['end'] != (offset + len(data) == size)):
                    raise ProtocolError('Video export returned an invalid byte range')
                output.write(data)
                offset += len(data)
            output.flush()
            os.fsync(output.fileno())
        if overwrite:
            os.replace(temporary, destination)
        else:
            os.link(temporary, destination)
            temporary.unlink()
        temporary = None
        return {'path': str(destination), 'bytes': size, 'first_frame': first_frame, 'last_frame': last_frame}
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
        if ticket is not None and not disconnected:
            exchange(lambda: client.release_export(ticket))
