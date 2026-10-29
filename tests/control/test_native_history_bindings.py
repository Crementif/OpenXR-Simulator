"""SDK and Luau window lifecycles preserve exact composed/quad history."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from native_support import probe_environment, running_probe, require, wait_file
from simxr_control import Client, TargetKind


def main():
    ui = '--ui' in sys.argv
    target = 'quad_layers' if ui else 'composed_eyes'
    kind = TargetKind.QUAD_LAYERS if ui else TargetKind.COMPOSED_EYES
    luau_target = 'quad_layers' if ui else 'composed'
    with tempfile.TemporaryDirectory(prefix='sx-bindings-') as temporary:
        data = Path(temporary)
        environment = probe_environment(data, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_RECORDING='1', SIMXR_PROBE_DYNAMIC_COLOR='1',
                           SIMXR_PROBE_MOVING_QUAD='1', SIMXR_PROBE_FRAMES='28',
                           SIMXR_WIDTH='64', SIMXR_HEIGHT='48')
        with running_probe([sys.argv[1]], environment) as host:
            client, runner = None, None
            try:
                wait_file(data / 'control.ready', host)
                socket = data / f'control.{host.pid}.sock'
                client = Client(socket)
                session, = client.sessions()
                (data / 'control.continue').touch()
                for frame in range(3, 31):
                    wait_file(data / f'recording.{frame}.ready', host)
                    if frame == 3:
                        started = client.begin_image_window(session, target=kind)
                        window = started['image_window_id']
                        require(started['document']['trigger_frame'] == 3, started)
                    if frame == 15:
                        document = client.image_window(window)['document']
                        require(document['phase'] == 'complete' and document['post_count'] == 12 and document['target'] == target, document)
                        retained = client.retain_window_image(window, 3)
                        image = retained['image_reader_id']
                        client.release_image_window(window)
                        exported = client.start_export(image, layer=1 if ui else 0)
                        ticket = exported['export_id']
                        for _ in range(500):
                            status = client.export_info(ticket)
                            if status['phase'] == 'complete':
                                break
                            time.sleep(.002)
                        require(status['phase'] == 'complete', status)
                        chunk = client.read_export(ticket)
                        pixels = chunk['bytes']
                        check_pixels(pixels, ui)
                        client.release_export(ticket)
                        client.release_image(image)
                        history = client.image_history(session, target=kind, count=1)
                        require(history['document']['target'] == target, history)
                        retained = client.retain_historical_image(session, 15, target=kind)
                        client.release_image(retained['image_reader_id'])
                    if frame == 16:
                        rig = data / 'window.luau'
                        rig.write_text('''local options = {target="TARGET"}
    local history
    repeat
        simxr.wait(2)
        history = simxr.image_history(1, options)
    until history.entries[1].phase == "ready"
    local document, window = simxr.begin_image_window(options)
    assert(document.trigger_frame == "16")
    simxr.export_history("16", "admitted.rgba", SLICE, options)
    repeat
        simxr.wait(2)
        document = simxr.image_window(window)
        assert(document.phase ~= "interrupted")
        local ready = document.phase == "complete"
        for _, entry in document.entries do ready = ready and entry.phase == "ready" end
    until ready
    assert(document.entries[1].frame_id == "7" and document.entries[22].frame_id == "28")
    simxr.export_window_image(window, "7", "window.rgba", SLICE)
    local analysis = simxr.analyze_image_window(window)
    if options.target == "quad_layers" then
        assert(analysis.frames_analyzed == 0 and analysis.frames_skipped == 22 and analysis.temporal_pairs == 0)
        assert(#analysis.skipped_frames == 22 and not analysis.detected)
        for _, skipped in analysis.skipped_frames do assert(skipped.reason == "missing_eye") end
    else
        assert(analysis.frames_analyzed == 22 and analysis.frames_skipped == 0 and analysis.temporal_pairs == 21)
        assert(#analysis.skipped_frames == 0)
    end
    assert(analysis.frame_gaps == 0 and type(analysis.detected) == "boolean")
    for _, event in analysis.events do
        assert(not simxr.frame_before(event.frame_id, "7") and not simxr.frame_before("28", event.frame_id))
        assert(type(event.reason) == "string" and #event.reason > 0)
    end
    simxr.release_image_window(window)
    simxr.checkpoint("Exact historical window exported", true)
    '''.replace('TARGET', luau_target).replace('SLICE', '1' if ui else '0'))
                        runner = subprocess.Popen([sys.argv[2], '--socket', str(socket), '--rig', str(rig),
                                                   '--json', str(data / 'report.json')], cwd=data, env=environment,
                                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                        wait_file(data / 'admitted.rgba', runner)
                    if frame == 28:
                        output = runner.communicate(timeout=10)[0]
                        require(runner.returncode == 0, output)
                        require(json.loads((data / 'report.json').read_text())['phase'] == 'complete', output)
                        check_pixels((data / 'window.rgba').read_bytes(), ui)
                        check_pixels((data / 'admitted.rgba').read_bytes(), ui, b'\0\xff\0')
                        with Client(socket) as observer:
                            session, = observer.sessions()
                            fresh = observer.begin_image_window(session)['image_window_id']
                            observer.release_image_window(fresh)
                    (data / f'recording.{frame}.continue').touch()
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
                output = host.communicate(timeout=15)[0]
                require(host.returncode == 0 and 'VUID-' not in output and 'Vulkan validation error' not in output, output)
                print('PASS SDK/Luau historical target selection, window ownership and exact native pixels')
            finally:
                if client is not None:
                    client.close()
                if runner is not None and runner.poll() is None:
                    runner.kill()
                    print(runner.communicate()[0])


def check_pixels(pixels, ui, expected=b'\xff\0\0'):
    require(len(pixels) == 64 * 48 * 4, 'Wrong exported extent')
    colors = {pixels[index:index+3] for index in range(0, len(pixels), 4)}
    require(colors == ({expected, b'\0\0\0'} if ui else {expected}), 'Historical source pixels were substituted')


if __name__ == '__main__':
    main()
