"""Public OpenXR slow-source rotation, compared with repeat at display cadence."""
import math
import os
from pathlib import Path
import sys
import tempfile
import time

from native_support import probe_environment, running_probe, export, require, wait_file
from simxr_control import Client
from simxr_control.protocol import Capability


MINIFIED = bool(os.getenv('SIMXR_PROBE_MIP_MINIFY'))
WIDTH, HEIGHT = (16, 16) if MINIFIED else (64, 48)
LEFT = (0, 255, 0, 255) if MINIFIED else (255, 0, 0, 255)
RIGHT = (255, 255, 0, 255) if MINIFIED else (0, 0, 255, 255)


def multiply(a, b):
    x, y, z, w = a
    u, v, t, s = b
    return [w*u + x*s + y*t - z*v, w*v - x*t + y*s + z*u,
            w*t + x*v - y*u + z*s, w*s - x*u - y*v - z*t]


def pixels_for_eye(fov, ipd, eye, yaw):
    left, right, up, down = map(math.tan, fov)
    cosine, sine = math.cos(yaw), math.sin(yaw)
    pixels = bytearray()
    for y in range(HEIGHT):
        ray_y = up + (y + .5) / HEIGHT * (down - up)
        for x in range(WIDTH):
            ray_x = left + (x + .5) / WIDTH * (right - left)
            source_x = cosine * ray_x - sine
            source_z = -sine * ray_x - cosine
            visible = (source_z < 0 and left <= source_x / -source_z <= right
                       and down <= ray_y / -source_z <= up)
            color = LEFT if eye == 0 else RIGHT
            if not visible:
                color = (0, 0, 0, 255)
            if eye == 1 and abs(ipd / 2 + 2 * ray_x) < .5 and abs(2 * ray_y) < .5:
                color = LEFT
            pixels.extend(color)
    return pixels


def selected(client, session, frame):
    deadline = time.monotonic() + 5
    while True:
        result = client.retain_preview(session)
        if result.get('frame', {}).get('frame_id') == frame:
            return result
        if result.get('image_reader_id'):
            client.release_image(result['image_reader_id'])
        require(time.monotonic() < deadline, result)
        time.sleep(.002)


def verify_pixels(client, reader, views, yaw):
    for eye in range(2):
        metadata, pixels = export(client, reader, eye)
        expected = pixels_for_eye(views['outer_fov'][eye*4:eye*4+4], views['ipd'], eye, yaw)
        require(len(pixels) == len(expected), metadata)
        wrong = [i // 4 for i in range(0, len(pixels), 4) if pixels[i:i+4] != expected[i:i+4]]
        require(not wrong, f'Eye {eye}, yaw {yaw}: {len(wrong)} wrong pixels; first {[(i, list(pixels[i*4:i*4+4]), list(expected[i*4:i*4+4])) for i in wrong[:8]]}')


def run(probe, policy):
    with tempfile.TemporaryDirectory(prefix='sx-rotate-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_DETERMINISTIC='0',
                           SIMXR_WIDTH=str(WIDTH), SIMXR_HEIGHT=str(HEIGHT), SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_GPU_INSPECTION='1', SIMXR_PROBE_RECORDING='1',
                           SIMXR_PROBE_RENDER_GATE='1', SIMXR_PROBE_WORLD_PROJECTION='1',
                           SIMXR_PROBE_INTERACTIVE='1', SIMXR_PROBE_FRAMES='6',
                           SIMXR_PRESENTATION_POLICY=policy)
        if policy == 'repeat':
            environment.pop('SIMXR_PRESENTATION_POLICY')
        with running_probe([probe], environment) as host:
            wait_file(data / 'control.ready', host)
            with Client(data / f'control.{host.pid}.sock') as client:
                session, = client.sessions()
                status = client.status(session)
                require(status['compatibility_deviations'] == [], status)
                require(status['capabilities'] & Capability.ROTATIONAL_REPROJECTION, status)
                require(not client.hello['capabilities'] & Capability.ROTATIONAL_REPROJECTION,
                        'Session-specific support must not be promised by Hello')
                (data / 'control.continue').touch()
                for frame in range(3, 7):
                    wait_file(data / f'rendering.{frame}.ready', host)
                    (data / f'rendering.{frame}.continue').touch()
                    wait_file(data / f'recording.{frame}.ready', host)
                    if frame < 6:
                        (data / f'recording.{frame}.continue').touch()
                baseline = selected(client, session, 6)
                state = client.simulation_status(session)['document']['configuration']
                views, head = state['views'], state['head']
                verify_pixels(client, baseline['image_reader_id'], views, 0)
                yaw = .25
                orientation = multiply(head['orientation'], [0, math.sin(yaw/2), 0, math.cos(yaw/2)])
                transaction = client.schedule(session, [
                    {'kind': 'take_control', 'channel': 'head', 'replace': True},
                    {'kind': 'head_pose', 'orientation': orientation, 'position': head['position']}], frame=7)
                require(transaction['phase'] == 'queued', transaction)
                (data / 'recording.6.continue').touch()
                wait_file(data / 'rendering.7.ready', host)
                applied = client.wait_simulation(session, transaction['transaction_id'])
                require(applied['phase'] == 'applied', applied)
                start = time.monotonic()
                samples = []
                while True:
                    status = client.status(session)
                    tick = status['presentation']['display']
                    require(status['presentation']['last_submitted'] == 6, status)
                    require(tick['source']['frame_id'] == 6 and tick['presentation_policy'] == policy, tick)
                    samples.append(tick)
                    elapsed = time.monotonic() - start
                    if elapsed >= .2 and (policy == 'repeat' or tick['reprojected']):
                        break
                    require(elapsed < 5, 'Rotational output did not complete')
                    time.sleep(.001)
                current = selected(client, session, 6)
                tick = current['display']
                try:
                    require(tick['tick'] >= samples[0]['tick'] + 3, 'Display stopped with the application')
                    require(tick['time'] > samples[0]['time'], 'Display time did not advance')
                    if policy == 'rotational':
                        require(tick['reprojected'] and tick['reprojection_result'] == 0, tick)
                        require(tick['image_tick'] > baseline['display']['image_tick'], tick)
                        require(max(abs(a-b) for a, b in zip(tick['display_head']['orientation'], orientation)) < 1e-5, tick)
                        require(tick['display_head']['position'] == baseline['display']['display_head']['position'], tick)
                        versions = {item['image_tick'] for item in samples if item['reprojected'] and
                                    max(abs(a-b) for a, b in zip(item['display_head']['orientation'], orientation)) < 1e-5}
                        require(versions == {tick['image_tick']}, 'Unchanged tracking repeatedly replaced the image')
                        verify_pixels(client, current['image_reader_id'], views, yaw)
                    else:
                        require(not tick['reprojected'] and tick['image_tick'] == baseline['display']['image_tick'], tick)
                        verify_pixels(client, current['image_reader_id'], views, 0)
                    verify_pixels(client, baseline['image_reader_id'], views, 0)
                    enqueue = max(item['reprojection_submission_time'] for item in samples)
                    completion = max(item['reprojection_completion_latency'] for item in samples)
                    print(f'{policy}: source=6, ticks={samples[0]["tick"]}..{tick["tick"]}, '
                          f'imageTick={baseline["display"]["image_tick"]}->{tick["image_tick"]}, '
                          f'enqueueCpuUs={enqueue/1000:.1f}, observedCompletionUs={completion/1000:.1f}', flush=True)
                finally:
                    client.release_image(current['image_reader_id'])
                    client.release_image(baseline['image_reader_id'])
                for frame in (7, 8):
                    wait_file(data / f'rendering.{frame}.ready', host)
                    (data / f'rendering.{frame}.continue').touch()
                    wait_file(data / f'recording.{frame}.ready', host)
                    (data / f'recording.{frame}.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0, output)
            require('Validation Error' not in output, output)


if __name__ == '__main__':
    for policy in ('repeat', 'rotational'):
        run(sys.argv[1], policy)
