import pathlib
import subprocess
import sys
import tempfile
import unittest


HOST = pathlib.Path(sys.argv.pop(1)).resolve()


class HostCliTests(unittest.TestCase):
    def run_host(self, *args):
        return subprocess.run([str(HOST), *map(str, args)], capture_output=True, text=True, timeout=10)

    def test_help_does_not_require_runtime(self):
        result = self.run_host('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--runtime MANIFEST', result.stdout)

    def test_trackers_require_input(self):
        result = self.run_host('--forward-trackers')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires --forward-input', result.stderr)

    def test_body_selection(self):
        for provider in ('fb-upper', 'meta-full', 'bd-without-arms', 'bd-full'):
            result = self.run_host('--forward-body', provider)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('requires --forward-input', result.stderr)
            result = subprocess.run([str(HOST), '--run', '--parent-control',
                                     '--forward-input', 'endpoint', '1', '1',
                                     '--forward-body', provider],
                                    input='', capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
        for extra in ((), ('unknown',), ('fb-upper', '--forward-body', 'bd-full')):
            result = self.run_host('--forward-body', *extra)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('OpenXR-Loader', result.stderr)
        result = self.run_host('--discover', '--parent-control', '--forward-body', 'fb-upper')
        self.assertIn('Discovery requires', result.stderr)

    def test_parent_control(self):
        result = self.run_host('--parent-control')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires --test-frames', result.stderr)
        for value in ('', 'stop\n'):
            result = subprocess.run([str(HOST), '--test-frames', '1', '--parent-control'],
                                    input=value, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn('OpenXR-Loader', result.stderr)
        result = subprocess.run([str(HOST), '--test-frames', '1', '--parent-control'],
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('standard-input pipe', result.stderr)

    def test_discovery_mode(self):
        for extra in ((), ('--parent-control', '--run'), ('--parent-control', '--probe'),
                      ('--parent-control', '--test-frames', '1'),
                      ('--parent-control', '--forward-input', 'endpoint', '1', '1'),
                      ('--parent-control', '--game-output')):
            result = self.run_host('--discover', *extra)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Discovery requires', result.stderr)
            self.assertNotIn('OpenXR-Loader', result.stderr)
        result = subprocess.run([str(HOST), '--discover', '--parent-control'],
                                input='', capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('OpenXR-Loader', result.stderr)

    def test_continuous_mode(self):
        for args in (('--take-over',), ('--run',), ('--run', '--parent-control'), ('--run', '--probe'),
                     ('--run', '--test-frames', '2'), ('--probe', '--test-frames', '2')):
            result = self.run_host(*args)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('OpenXR-Loader', result.stderr)
        result = subprocess.run([str(HOST), '--run', '--parent-control',
                                 '--forward-input', 'endpoint', '1', '1'],
                                input='', capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('OpenXR-Loader', result.stderr)

    def test_game_output_requires_input_target(self):
        for args in (('--game-output',), ('--game-output', '--test-frames', '2'),
                     ('--game-output', '--probe')):
            result = self.run_host(*args)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('OpenXR-Loader', result.stderr)
        result = subprocess.run([str(HOST), '--run', '--game-output', '--parent-control',
                                 '--forward-input', 'endpoint', '1', '1'],
                                input='', capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_conflicting_runtime_options(self):
        result = self.run_host('--system', '--runtime', 'anything.json')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Expected --system', result.stderr)

    def test_invalid_frame_count(self):
        for value in ('0', '-1', '10001', '2x'):
            with self.subTest(value=value):
                result = self.run_host('--test-frames', value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Test frame count', result.stderr)
                self.assertNotIn('OpenXR-Loader', result.stderr)

    def test_invalid_image_source(self):
        for args in (('endpoint',), ('endpoint', '0', '1', '1'),
                     ('endpoint', '1', '-1', '1'), ('endpoint', '1', '1', '2x'),
                     ('endpoint', '18446744073709551616', '1', '1'), ('', '1', '1', '1')):
            with self.subTest(args=args):
                result = self.run_host('--copy-image', *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Image copy', result.stderr)
                self.assertNotIn('OpenXR-Loader', result.stderr)

    def test_invalid_live_image_source(self):
        for args in (('endpoint',), ('endpoint', '0', '1'), ('endpoint', '1', '-1'),
                     ('endpoint', '1', '2x'), ('', '1', '1')):
            result = self.run_host('--copy-live-image', *args)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Image copy', result.stderr)
            self.assertNotIn('OpenXR-Loader', result.stderr)
        result = subprocess.run([str(HOST), '--copy-live-image', 'endpoint', '1', '1',
                                 '--test-frames', '1', '--parent-control'],
                                input='', capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_input_target(self):
        for args in (('endpoint',), ('endpoint', '0', '1'), ('', '1', '1'),
                     ('endpoint', '1', '2x'), ('endpoint', '1', '1')):
            result = self.run_host('--forward-input', *args)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Input forwarding', result.stderr)
            self.assertNotIn('OpenXR-Loader', result.stderr)

    def test_alignment_arguments(self):
        for args in (('--align-player', '0'), ('--align-player', 'nan', '0', '0', '0'),
                     ('--align-player', '0', '0', '0', '0'), ('--local-eye-height', '1.7'),
                     ('--local-eye-height', '-1')):
            with self.subTest(args=args):
                result = self.run_host(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('OpenXR-Loader', result.stderr)

    def test_manifest_alias_rejected_before_loader(self):
        with tempfile.TemporaryDirectory(prefix='simxr-headset-cli-') as directory:
            manifest = pathlib.Path(directory) / 'physical.json'
            manifest.write_text('{"runtime":{"library_path":"libopenxr_simulator.so"}}')
            result = self.run_host('--runtime', manifest, '--probe')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('points to the simulator', result.stderr)
            self.assertNotIn('OpenXR-Loader', result.stderr)


if __name__ == '__main__':
    unittest.main()
