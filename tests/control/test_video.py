"""Video save failures must not replace files or leave export tickets alive."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Session, ProtocolError
from simxr_control.video import save_video


class ExportClient:
    def __init__(self, *, failed=False, truncated=False):
        self.failed, self.truncated = failed, truncated
        self.released = False

    def start_video_export(self, *args):
        return {'export_id': 7}

    def export_info(self, ticket):
        return {'phase': 'failed' if self.failed else 'complete', 'detail': 'GPU failed',
                'total_bytes': 8, 'media_type': 'video/mp4'}

    def read_export(self, ticket, offset, length):
        return {'offset': offset, 'length': 4 if self.truncated else 8,
                'bytes': b'half' if self.truncated else b'complete', 'end': True}

    def release_export(self, ticket):
        self.released = True


class VideoTests(unittest.TestCase):
    def test_atomic_save_and_existing_destination(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'clip.mp4'
            client = ExportClient()
            result = save_video(client, Session(1, 1), 3, 4, path)
            self.assertEqual(path.read_bytes(), b'complete')
            self.assertEqual(result['bytes'], 8)
            self.assertTrue(client.released)
            with self.assertRaises(FileExistsError):
                save_video(client, Session(1, 1), 3, 4, path)

    def test_failures_leave_original_file_and_release_ticket(self):
        for option, error in [('failed', RuntimeError), ('truncated', ProtocolError)]:
            with self.subTest(option=option), tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / 'clip.mp4'
                path.write_bytes(b'original')
                client = ExportClient(**{option: True})
                with self.assertRaises(error):
                    save_video(client, Session(1, 1), 3, 4, path, overwrite=True)
                self.assertEqual(path.read_bytes(), b'original')
                self.assertEqual(list(Path(folder).iterdir()), [path])
                self.assertTrue(client.released)

    def test_mcp_video_schema_and_dispatch(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('video_debug', Path(__file__).resolve().parents[2] / 'mcp-server/debug_control.py')
        adapter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adapter)
        args = adapter._validate('debug_save_video', dict(connection='c', session='1', generation='1',
            first_frame='3', last_frame='5', path='clip.mp4'))
        self.assertEqual(args['first_frame'], 3)
        self.assertIn('debug_start_video_export', adapter._SCHEMAS)

if __name__ == '__main__':
    unittest.main()
