import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'sdk/python'))
from simxr_control import (CommandKind, ControlError, ControlTimeout, ErrorCode, Lease, Session, TargetKind)

spec = importlib.util.spec_from_file_location('debug_control_adapter', ROOT / 'mcp-server/debug_control.py')
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)
HOST = None


class FakeClient:
    def __init__(self, path, **options):
        self.path, self.options = path, options
        self.hello = {'error': 0, 'capabilities': 7}
        self.calls = []
        self.closed = False
        self.timeout_next = False
        self.pending = False
        self.fail_next = None
        self.status_hook = None

    def sessions(self):
        return [Session(2**63 + 1, 2)]

    def close(self):
        self.closed = True

    def _call(self, name, *args, **kwargs):
        self.calls.append((name, args, kwargs))
        if self.fail_next:
            failure, self.fail_next = self.fail_next, None
            raise failure
        if self.pending:
            raise ControlTimeout(99)
        if self.timeout_next:
            self.timeout_next = False
            self.pending = True
            raise ControlTimeout(99)
        return {'error': 0}

    def status(self, session):
        if self.status_hook:
            self.status_hook()
        return self._call('status', session)

    def acquire(self, session):
        self._call('acquire', session)
        return Lease(session, 2**63 + 7)

    def renew(self, lease):
        return self._call('renew', lease)

    def relinquish(self, lease):
        return self._call('relinquish', lease)

    def submit(self, lease, command, **kwargs):
        result = self._call('submit', lease, command, **kwargs)
        return {**result, 'operation_id': 88}

    def operation(self, session, operation):
        return self._call('operation', session, operation)

    def capture(self, session, capture):
        result = self._call('capture', session, capture)
        return {**result, 'phase': 'clear', 'image_available': False}

    def inspect(self, session, capture):
        return {**self._call('inspect', session, capture), 'inspection_id': 2**63 + 1,
                'document': {'view_settings': {'revision_id': 2**63 + 2}}}

    def revision(self, revision):
        return {**self._call('revision', revision), 'revision_id': revision, 'revision_kind': 'view_settings_v1'}

    def release_inspection(self, inspection):
        return {**self._call('release_inspection', inspection), 'inspection_id': inspection, 'released': True}

    def retain_image(self, session, capture, **selection):
        return {**self._call('retain_image', session, capture, **selection), 'image_reader_id': 2**63 + 3}

    def image_info(self, image):
        return {**self._call('image_info', image), 'image_reader_id': image,
                'image_descriptor': {'device_uuid': bytes(range(16)), 'driver_uuid': b'\xff' * 16}}

    def release_image(self, image):
        return self._call('release_image', image)

    def delivery_info(self, delivery):
        return {**self._call('delivery_info', delivery), 'delivery_id': delivery, 'pool_generation': 2**64 - 1}

    def release_delivery(self, delivery):
        return self._call('release_delivery', delivery)

    def start_export(self, image, layer=None):
        return {**self._call('start_export', image, layer=layer), 'export_id': 2**63 + 4, 'phase': 'pending'}

    def export_info(self, export):
        return self._call('export_info', export)

    def read_export(self, export, offset=0, length=1):
        return {**self._call('read_export', export, offset=offset, length=length), 'export_id': export,
                'offset': offset, 'bytes': b'\x00\xff', 'end': True}

    def release_export(self, export):
        return self._call('release_export', export)

    def acknowledge(self, lease, through):
        return self._call('acknowledge', lease, through)

    def retry(self):
        self.calls.append(('retry', (), {}))
        if not self.pending:
            raise ValueError('No unfinished exchange')
        self.pending = False
        return {'error': 0, 'operation_id': 88}


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.created = []

        def factory(*args, **kwargs):
            client = FakeClient(*args, **kwargs)
            self.created.append(client)
            return client

        self.tools = adapter.DebugTools(max_connections=2, client_factory=factory)
        connected = self.tools.call('debug_connect', {'path': '/unused/mock.sock'})
        self.assertTrue(connected['ok'], connected)
        self.connection = connected['connection']
        self.session = connected['sessions'][0]
        self.common = {'connection': self.connection, **self.session}
        acquired = self.tools.call('debug_acquire', self.common)
        self.leased = {**self.common, 'lease': acquired['lease']}
        self.client = self.created[0]
        self.client.calls.clear()

    def tearDown(self):
        self.tools.close()

    def test_schemas_require_explicit_mutation_identity(self):
        definitions = {definition['name']: definition for definition in adapter.TOOL_DEFINITIONS}
        for name in ('debug_pause', 'debug_step', 'debug_strict_admission', 'debug_display_step',
                     'debug_resume', 'debug_cancel', 'debug_release'):
            self.assertTrue({'connection', 'session', 'generation', 'lease', 'command_id'} <=
                            set(definitions[name]['inputSchema']['required']))
        for command in ({**self.leased}, {**self.leased, 'command_id': True},
                        {**self.leased, 'command_id': 0}, {**self.leased, 'command_id': 2**64},
                        {**self.leased, 'command_id': 1, 'lookahead': 5},
                        {**self.leased, 'command_id': 1, 'target': 'composed_eyes', 'view': 1}):
            self.assertEqual(self.tools.call('debug_step', command)['error'], 'invalid_arguments')
        self.assertEqual(self.client.calls, [])
        self.assertTrue(self.tools.call('debug_status', {**self.common, 'generation': 0})['ok'])
        self.assertEqual(self.tools.call('debug_export_image', self.common)['error'], 'unsupported_tool')
        self.assertEqual(self.tools.call('debug_capture', {**self.common, 'capture': 1, 'filename': 'legacy.png'})['error'],
                         'invalid_arguments')

    def test_realtime_recording_identity_and_budget(self):
        result = self.tools.call('debug_start_image_recording', {**self.leased, 'command_id': '42',
                                'target': 'layer_depth', 'view': 1, 'capacity': 2, 'frames': '0'})
        self.assertTrue(result['ok'], result)
        method, arguments, keywords = self.client.calls[-1]
        self.assertEqual(arguments[1], CommandKind.START_IMAGE_RECORDING)
        self.assertEqual(keywords, {'command_id': 42, 'target': TargetKind.LAYER_DEPTH, 'layer': 0, 'view': 1,
                                   'recording_capacity': 2, 'recording_frames': 0})
        self.assertEqual(self.tools.call('debug_start_image_recording', {**self.leased, 'command_id': 43,
                                                                      'capacity': 5})['error'], 'invalid_arguments')

    def test_exact_depth_selection(self):
        for name in ('layer_depth', 'layer_depth_linear'):
            result = self.tools.call('debug_retain_image', {**self.common, 'capture': '42',
                                    'target': name, 'layer': 2, 'view': 1})
            self.assertTrue(result['ok'], result)
            method, arguments, keywords = self.client.calls[-1]
            self.assertEqual(method, 'retain_image')
            self.assertEqual(arguments[-1], 42)
            self.assertEqual(keywords, {'target': TargetKind[name.upper()], 'layer': 2, 'view': 1})
        definition = next(item for item in adapter.TOOL_DEFINITIONS if item['name'] == 'debug_retain_image')
        self.assertNotIn('default', definition['inputSchema']['properties']['target'])
        self.assertNotIn('target', definition['inputSchema']['required'])

    def test_forwarding_and_metadata_only(self):
        commands = [('debug_pause', CommandKind.PAUSE), ('debug_step', CommandKind.STEP),
                    ('debug_strict_admission', CommandKind.STRICT_ADMISSION),
                    ('debug_display_step', CommandKind.DISPLAY_STEP), ('debug_resume', CommandKind.RESUME),
                    ('debug_cancel', CommandKind.CANCEL), ('debug_release', CommandKind.RELEASE)]
        for index, (name, kind) in enumerate(commands, 1):
            arguments = {**self.leased, 'command_id': str(2**63 + index)}
            if name in ('debug_step', 'debug_strict_admission'):
                arguments.update(target='layer_view', layer=3, view=2, lookahead=4)
            if name == 'debug_cancel':
                arguments['operation'] = 44
            if name == 'debug_release':
                arguments['capture'] = 55
            self.assertTrue(self.tools.call(name, arguments)['ok'])
            called, positional, keywords = self.client.calls[-1]
            self.assertEqual(called, 'submit')
            self.assertEqual(positional, (Lease(Session(int(self.session['session']), 2), int(self.leased['lease'])), kind))
            self.assertEqual(keywords['command_id'], 2**63 + index)
            if name in ('debug_step', 'debug_strict_admission'):
                self.assertEqual(keywords, {'command_id': 2**63 + index, 'target': TargetKind.LAYER_VIEW,
                                           'layer': 3, 'view': 2, 'lookahead': 4})
        for name in ('debug_status', 'debug_renew', 'debug_relinquish'):
            self.assertTrue(self.tools.call(name, self.common if name == 'debug_status' else self.leased)['ok'])
        self.assertTrue(self.tools.call('debug_operation', {**self.common, 'operation': 88})['ok'])
        captured = self.tools.call('debug_capture', {**self.common, 'capture': 55})
        self.assertTrue(captured['ok'] and captured['metadata_only'])
        self.assertFalse(captured['gpu_preview_supported'])
        self.assertNotIn('bytes', captured)
        inspected = self.tools.call('debug_inspect', {**self.common, 'capture': 55})
        self.assertTrue(inspected['ok'] and inspected['metadata_only'])
        self.assertEqual(inspected['inspection_id'], str(2**63 + 1))
        revision_id = inspected['document']['view_settings']['revision_id']
        self.assertEqual(revision_id, str(2**63 + 2))
        self.assertTrue(self.tools.call('debug_revision', {'connection': self.connection, 'revision': revision_id})['ok'])
        self.assertEqual(self.client.calls[-1][1], (2**63 + 2,))
        self.assertTrue(self.tools.call('debug_release_inspection', {'connection': self.connection, 'inspection': inspected['inspection_id']})['released'])
        self.assertEqual(self.client.calls[-1][1], (2**63 + 1,))
        self.assertTrue(self.tools.call('debug_acknowledge', {**self.leased, 'through': str(2**64 - 1)})['ok'])
        self.assertEqual(self.client.calls[-1][2], {})
        self.assertEqual(self.client.calls[-1][1][1], 2**64 - 1)

    def test_explicit_image_export_never_reads_during_metadata(self):
        import base64
        retained = self.tools.call('debug_retain_image', {**self.common, 'capture': '18446744073709551615'})
        self.assertEqual(retained['image_reader_id'], str(2**63 + 3))
        image = retained['image_reader_id']
        info = self.tools.call('debug_image_info', {'connection': self.connection, 'image': image})
        self.assertEqual(info['image_descriptor']['device_uuid'], bytes(range(16)).hex())
        started = self.tools.call('debug_start_export', {'connection': self.connection, 'image': image, 'layer': 1})
        self.assertEqual(started['phase'], 'pending')
        export = started['export_id']
        self.assertEqual(export, str(2**63 + 4))
        self.tools.call('debug_export_info', {'connection': self.connection, 'export': export})
        self.assertNotIn('read_export', [call[0] for call in self.client.calls])
        read = self.tools.call('debug_read_export', {'connection': self.connection, 'export': export,
                                                   'offset': '18446744073709551615', 'length': 2})
        self.assertEqual(base64.b64decode(read['bytes']['data']), b'\x00\xff')
        self.assertEqual(read['bytes']['encoding'], 'base64')
        self.assertEqual(self.client.calls[-1], ('read_export', (2**63 + 4,), {'offset': 2**64 - 1, 'length': 2}))
        for bad in ({'offset': 2**53}, {'offset': '00'}, {'length': 0}, {'length': 1024 * 1024}):
            self.assertEqual(self.tools.call('debug_read_export', {'connection': self.connection, 'export': export, **bad})['error'],
                             'invalid_arguments')
        self.assertTrue(self.tools.call('debug_release_export', {'connection': self.connection, 'export': export})['ok'])
        self.assertTrue(self.tools.call('debug_release_image', {'connection': self.connection, 'image': image})['ok'])
        delivery = self.tools.call('debug_delivery_info', {'connection': self.connection, 'delivery': '18446744073709551615'})
        self.assertEqual(delivery['delivery_id'], str(2**64 - 1))
        self.assertEqual(delivery['pool_generation'], str(2**64 - 1))
        self.assertTrue(self.tools.call('debug_release_delivery', {'connection': self.connection, 'delivery': delivery['delivery_id']})['ok'])
        self.assertEqual(self.tools.call('debug_share_image', {'connection': self.connection})['error'], 'unsupported_tool')
        self.assertNotIn('submit', [call[0] for call in self.client.calls])

    def test_full_width_canonical_identifiers_and_json_response(self):
        self.assertIsInstance(self.session['session'], str)
        self.assertEqual(self.session['session'], str(2**63 + 1))
        for value in (2**53, 2**63 + 1, True, '', '00', '01', '+1', '-1', ' 1', '1 ',
                      '1.0', '1e3', '１２', str(2**64), '0'):
            result = self.tools.call('debug_pause', {**self.leased, 'command_id': value})
            self.assertEqual(result['error'], 'invalid_arguments', value)
        self.assertEqual(self.client.calls, [])
        full = 2**64 - 1
        calls = []

        def submit(lease, command, **kwargs):
            calls.append((lease, command, kwargs))
            return {'operation_id': full, 'command_id': full, 'captures': [1, full],
                    'diagnostics': [full], 'frame': {'frame_id': full, 'generation': 0},
                    'display': {'tick': full}, 'pipeline': {'next_admission': full},
                    'presentation': {'last_submitted': full}, 'time': 2**60, 'count': 17}

        self.client.submit = submit
        response = self.tools.call('debug_pause', {**self.leased, 'session': str(full),
                                   'generation': str(full), 'lease': str(full), 'command_id': str(full)})
        self.assertTrue(response['ok'], response)
        self.assertEqual(calls[0], (Lease(Session(full, full), full), CommandKind.PAUSE, {'command_id': full}))
        self.assertEqual(response['operation_id'], str(full))
        self.assertEqual(response['command_id'], str(full))
        self.assertEqual(response['captures'], ['1', str(full)])
        self.assertEqual(response['diagnostics'], [str(full)])
        self.assertEqual(response['frame'], {'frame_id': str(full), 'generation': '0'})
        self.assertEqual(response['display']['tick'], str(full))
        self.assertEqual(response['pipeline']['next_admission'], str(full))
        self.assertEqual(response['presentation']['last_submitted'], str(full))
        self.assertEqual(response['time'], str(2**60))
        self.assertEqual(response['count'], 17)

    def test_timeout_and_retry_never_resubmits(self):
        self.client.timeout_next = True
        result = self.tools.call('debug_step', {**self.leased, 'command_id': 19})
        self.assertFalse(result['ok'])
        self.assertEqual(result['error'], 'unfinished')
        self.assertEqual(result['request_id'], '99')
        self.assertTrue(result['retry_needed'])
        retried = self.tools.call('debug_retry', {'connection': self.connection})
        self.assertTrue(retried['ok'])
        self.assertEqual(retried['operation_id'], '88')
        self.assertEqual([call[0] for call in self.client.calls], ['submit', 'retry'])
        self.client.fail_next = ControlError({'error': int(ErrorCode.STALE_GENERATION), 'detail': 'generation changed'})
        failed = self.tools.call('debug_status', self.common)
        self.assertEqual(failed['error'], 'control_error')
        self.assertEqual(failed['code'], int(ErrorCode.STALE_GENERATION))
        self.assertEqual(failed['record']['detail'], 'generation changed')
        self.client.fail_next = ConnectionError('peer closed')
        self.assertEqual(self.tools.call('debug_status', self.common)['error'], 'transport_error')

    def test_global_registry_is_not_held_across_ipc(self):
        other = self.tools.call('debug_connect', {'path': '/unused/other.sock'})
        entered, release = threading.Event(), threading.Event()
        self.client.status_hook = lambda: (entered.set(), release.wait(2))
        responses = []
        worker = threading.Thread(target=lambda: responses.append(self.tools.call('debug_status', self.common)))
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            self.assertTrue(self.tools.call('debug_status', {'connection': other['connection'], **self.session})['ok'])
            self.assertTrue(self.tools.call('debug_disconnect', {'connection': other['connection']})['ok'])
        finally:
            release.set()
            worker.join(2)
        self.assertFalse(worker.is_alive())
        self.assertTrue(responses[0]['ok'])

    def test_capacity_and_close(self):
        self.assertTrue(self.tools.call('debug_connect', {})['ok'])
        self.assertEqual(self.tools.call('debug_connect', {})['error'], 'capacity')
        self.assertEqual(len(self.created), 2)
        self.tools.close()
        self.tools.close()
        self.assertTrue(all(client.closed for client in self.created))
        self.assertEqual(self.tools.call('debug_connect', {})['error'], 'closed')

    def test_connect_timeout_and_discovery_retry(self):
        attempts = []

        def factory(*args, **kwargs):
            attempts.append(1)
            if len(attempts) == 1:
                raise ControlTimeout(1)
            client = FakeClient(*args, **kwargs)
            client.sessions = lambda: (_ for _ in ()).throw(ControlTimeout(2))
            client.pending = True
            return client

        tools = adapter.DebugTools(max_connections=1, client_factory=factory)
        try:
            negotiation = tools.call('debug_connect', {})
            self.assertFalse(negotiation['retry_needed'])
            self.assertTrue(negotiation['reconnect_needed'])
            discovery = tools.call('debug_connect', {})
            self.assertEqual(discovery['error'], 'unfinished')
            self.assertTrue(discovery['retry_needed'])
            self.assertIn('connection', discovery)
            self.assertTrue(tools.call('debug_retry', {'connection': discovery['connection']})['ok'])
            self.assertEqual(len(attempts), 2)
            self.assertEqual(tools.call('debug_connect', {})['error'], 'capacity')
        finally:
            tools.close()
        self.assertEqual(self.tools.call('debug_connect', {'timeout': 2**4096})['error'], 'invalid_arguments')

    def test_module_import_does_not_load_sdk_or_mcp(self):
        code = ("import importlib.util,sys; "
                f"s=importlib.util.spec_from_file_location('adapter', {str(ROOT / 'mcp-server/debug_control.py')!r}); "
                "m=importlib.util.module_from_spec(s);s.loader.exec_module(m); "
                "assert 'simxr_control' not in sys.modules;assert 'mcp' not in sys.modules; "
                "assert m.TOOL_DEFINITIONS")
        subprocess.run([sys.executable, '-I', '-c', code], check=True)

    def test_pending_connect_reserves_capacity_and_close_cancels_publication(self):
        entered, release = threading.Event(), threading.Event()
        created = []

        def factory(*args, **kwargs):
            entered.set()
            release.wait(2)
            client = FakeClient(*args, **kwargs)
            created.append(client)
            return client

        tools = adapter.DebugTools(max_connections=1, client_factory=factory)
        responses = []
        worker = threading.Thread(target=lambda: responses.append(tools.call('debug_connect', {})))
        worker.start()
        try:
            self.assertTrue(entered.wait(1))
            self.assertEqual(tools.call('debug_connect', {})['error'], 'capacity')
            tools.close()
        finally:
            release.set()
            worker.join(2)
        self.assertEqual(responses[0]['error'], 'closed')
        self.assertEqual(len(created), 1)
        self.assertTrue(created[0].closed)


@unittest.skipUnless(HOST, 'C++ endpoint host was not supplied')
class EndpointTests(unittest.TestCase):
    def test_real_adapter_exact_frames_and_disconnect_recovery(self):
        with tempfile.TemporaryDirectory(prefix='simxr-mcp-control-') as temporary:
            path, start = Path(temporary) / 'control.sock', Path(temporary) / 'start'
            process = subprocess.Popen([HOST, str(path), str(start)], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            tools = adapter.DebugTools()
            try:
                deadline = time.monotonic() + 5
                while not path.exists():
                    self.assertIsNone(process.poll())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(0.005)
                connected = tools.call('debug_connect', {'path': str(path)})
                self.assertTrue(connected['ok'], connected)
                session, = connected['sessions']
                common = {'connection': connected['connection'], **session}
                acquired = tools.call('debug_acquire', common)
                self.assertTrue(acquired['ok'], acquired)
                leased = {**common, 'lease': acquired['lease']}

                def wait(operation):
                    deadline = time.monotonic() + 5
                    while True:
                        record = tools.call('debug_operation', {**common, 'operation': operation})
                        self.assertTrue(record['ok'], record)
                        if record['phase'] not in ('queued', 'pending'):
                            return record
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.002)

                pause = tools.call('debug_pause', {**leased, 'command_id': 1})
                self.assertEqual(wait(pause['operation_id'])['progress'], 'paused')
                for index in range(1, 5):
                    command = {**leased, 'command_id': 10 + index}
                    step = tools.call('debug_step', command)
                    duplicate = tools.call('debug_step', command)
                    self.assertEqual(step['operation_id'], duplicate['operation_id'])
                    self.assertTrue(duplicate['duplicate'])
                    if index == 1:
                        start.touch()
                    ready = wait(step['operation_id'])
                    self.assertEqual(ready['progress'], 'clear')
                    capture = tools.call('debug_capture', {**common, 'capture': ready['capture_id']})
                    self.assertTrue(capture['ok'] and capture['metadata_only'])
                    self.assertEqual(capture['frame']['frame_id'], str(index))
                    self.assertEqual(capture['frame']['submission']['output_disposition'], 'clear')
                    inspected = tools.call('debug_inspect', {**common, 'capture': ready['capture_id']})
                    self.assertTrue(inspected['ok'] and inspected['metadata_only'], inspected)
                    self.assertEqual(inspected['document']['frame_id'], str(index))
                    revision = tools.call('debug_revision', {'connection': common['connection'], 'revision': inspected['document']['view_settings']['revision_id']})
                    self.assertTrue(revision['ok'], revision)
                    self.assertEqual(revision['revision_kind'], 'view_settings_v1')
                    self.assertTrue(tools.call('debug_release_inspection', {'connection': common['connection'], 'inspection': inspected['inspection_id']})['released'])
                self.assertEqual(tools.call('debug_status', common)['pipeline']['permits'], 0)
                # Close all control sockets: host frame five/six resume without
                # another debugger step, exercising lease recovery.
                tools.close()
                output = process.communicate(timeout=5)[0]
                self.assertEqual(process.returncode, 0, output)
                self.assertIn('PASS CPU control endpoint lifecycle', output)
            finally:
                tools.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if process.stdout:
                    process.stdout.close()


if __name__ == '__main__':
    if len(sys.argv) == 2 and not sys.argv[1].startswith('-'):
        HOST = sys.argv.pop(1)
        EndpointTests.__unittest_skip__ = False
    unittest.main()
