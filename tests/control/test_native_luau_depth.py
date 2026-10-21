"""Public Luau depth example against a real Vulkan capture and inspection lifetime."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_native_quad import require, wait_file
from simxr_control import Client


def main():
    with tempfile.TemporaryDirectory(prefix='sx-lua-depth-') as temporary:
        data = Path(temporary)
        environment = dict(os.environ, SIMXR_DATA_DIR=temporary, SIMXR_PROBE_CONTROL='1',
                           SIMXR_PROBE_RETAIN_CONTROL='1', SIMXR_PROBE_GPU_INSPECTION='1',
                           SIMXR_PROBE_DEPTH='1', SIMXR_PROBE_FRAMES='2')
        environment.pop('SIMXR_CONTROL_SOCKET', None)
        host = subprocess.Popen([sys.argv[1]], env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        runner = None
        try:
            wait_file(data / 'control.ready', host)
            example = Path(__file__).resolve().parents[2] / 'examples/rigs/inspect_depth.luau'
            source = example.read_text().replace('simxr.release_capture(capture)', '''
local absent = pcall(function()
    simxr.export(capture, "absent.rgba", {target="layer_view", layer=0, view=1})
end)
simxr.checkpoint("unretained color rejected", not absent)
assert(frame.frame_id == "3", "Depth came from a different frame")
simxr.release_capture(capture)''').replace('simxr.resume()', '''
local released = pcall(function() simxr.revision(frame.view_settings) end)
simxr.checkpoint("released revision rejected", not released)
simxr.resume()''')
            (data / 'example.luau').write_text(source)
            endpoint = data / f'control.{host.pid}.sock'
            runner = subprocess.Popen([sys.argv[2], '--socket', str(endpoint), '--rig', str(data / 'example.luau'),
                                      '--json', str(data / 'report.json'), '--junit', str(data / 'report.xml')],
                                     cwd=data, env=environment, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
            with Client(endpoint) as client:
                session, = client.sessions()
                deadline = time.monotonic() + 10
                while client.status(session)['pipeline']['permits'] != 1:
                    require(runner.poll() is None, runner.communicate()[0] if runner.poll() is not None else '')
                    require(time.monotonic() < deadline, 'Luau did not request its exact depth frame')
                    time.sleep(.005)
                (data / 'control.continue').touch()
                output = runner.communicate(timeout=15)[0]
                require(runner.returncode == 0, f"Runner exit {runner.returncode}: {output} " +
                        ((data / 'report.json').read_text() if (data / 'report.json').exists() else 'no report'))
                pixels = (data / 'depth.rgba').read_bytes()
                require(len(pixels) == 62 * 46 * 4, 'Luau exported the wrong depth extent/slice')
                for y in range(46):
                    for x in range(62):
                        value = .25 + .05 * ((x + 1) // 16) + .025 * ((y + 1) // 12)
                        expected = round((1.055 * value ** (1 / 2.4) - .055) * 255)
                        actual = pixels[(y * 62 + x) * 4:(y * 62 + x) * 4 + 4]
                        require(all(abs(channel - expected) <= 2 for channel in actual[:3]) and actual[3] == 255,
                                f'Wrong frame/crop/view at {x}, {y}: {list(actual)}')
                require(not (data / 'absent.rgba').exists(), 'Unretained target wrote a replacement image')
                report = json.loads((data / 'report.json').read_text())
                require(report['phase'] == 'complete', report)
                require((data / 'report.xml').stat().st_size > 0, 'Missing JUnit report')
                wait_file(data / 'control.destroyed', host)
                (data / 'control.retained.continue').touch()
            output = host.communicate(timeout=15)[0]
            require(host.returncode == 0, output)
            require('VUID-' not in output and 'Vulkan validation error' not in output, output)
            print('PASS actual Luau depth pixels, exact selection, independent revisions, cleanup and reports')
        finally:
            for process in (runner, host):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate()


if __name__ == '__main__':
    main()
