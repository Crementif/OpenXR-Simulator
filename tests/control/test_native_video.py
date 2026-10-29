"""Real Vulkan H.264 MP4 export, independently decoded using test-only FFmpeg tools."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


from native_support import probe_environment, running_probe, require, wait_file
from simxr_control import Client, Capability, ControlError


def verify(path, first, last):
    info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-count_frames',
        '-show_streams', '-of', 'json', str(path)]))['streams']
    require(len(info) == 1 and info[0]['codec_name'] == 'h264', info)
    stream = info[0]
    require(int(stream['nb_read_frames']) == last - first + 1, stream)
    width, height = stream['width'], stream['height']
    raw = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(path), '-fps_mode', 'passthrough', '-enc_time_base', '1:90000',
                                   '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'])
    require(len(raw) == width * height * 3 * (last - first + 1), 'Decoded size differs')
    for index, frame in enumerate(range(first, last + 1)):
        offset = (index * width * height + 24 * width + 32) * 3
        expected = (0, 0, 0) if frame < 3 else (0, 255, 0) if frame % 2 == 0 else (255, 0, 0)
        require(all(abs(a-b) <= 6 for a,b in zip(raw[offset:offset+3], expected)),
                f'Wrong image at frame {frame}: {tuple(raw[offset:offset+3])}')


def main(probe, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='simxr-video-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
            SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1', SIMXR_PROBE_RECORDING='1',
            SIMXR_PROBE_DYNAMIC_COLOR='1', SIMXR_PROBE_FRAMES='24', SIMXR_WIDTH='64', SIMXR_HEIGHT='48')

        with running_probe([probe], environment) as host:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                require(client.status(session)['capabilities'] & Capability.VIDEO_EXPORT, 'No video capability')
                client.configure_previews(session, 64, 64)
                (data / 'control.continue').touch()
                wait_file(data / 'recording.3.ready', host)
                client.save_video(session, 1, 3, destination / 'clear-and-recent.mp4', overwrite=True)
                verify(destination / 'clear-and-recent.mp4', 1, 3)
                for frame in range(3, 26):
                    (data / f'recording.{frame}.continue').touch()
                    wait_file(data / f'recording.{frame+1}.ready', host)
                status = client.status(session)
                require(status['preview_ram_bytes'] > 0 and not status['preview_error'], status)
                client.save_video(session, 3, 26, destination / 'archived-and-recent.mp4', overwrite=True)
                verify(destination / 'archived-and-recent.mp4', 3, 26)
                try:
                    client.start_video_export(session, 3, 999)
                except ControlError:
                    pass
                else:
                    raise AssertionError('Missing frames accepted')
                (data / 'recording.26.continue').touch()
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            (destination / 'native.log').write_text(output)
            require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS MP4 export: clear, recent and archived frames, exact ordering, independent decoding and teardown')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
