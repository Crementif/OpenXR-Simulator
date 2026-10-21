"""Owned real-time sequence playback and bounded exact-frame capture persistence."""
from collections import deque
from io import BytesIO
import json
import os
from pathlib import Path
import stat
import tempfile
import threading
import time

from retained_image import read_stereo_image


class CapturePersistenceError(RuntimeError):
    pass


TOOLS = frozenset({'run_pose_sequence', 'get_pose_sequence', 'stop_pose_sequence'})


class PoseSequenceControl:
    MAX_PROGRAM = 16 * 1024 * 1024
    MAX_PIXELS = 128 * 1024 * 1024
    MAX_DISK = 128 * 1024 * 1024
    MAX_RESULTS = 32

    def __init__(self, binding, lock, directory):
        self._binding, self._lock = binding, lock
        self._directory = Path(directory)
        self._output = None
        self._run = None
        self._pending = {}
        self._records = deque()
        self._bytes = 0
        self._evicted = 0
        self._closing = threading.Event()
        self._wake = threading.Event()
        self._worker = None
        self._manifest_bytes = None

    def close(self):
        self._closing.set()
        self._wake.set()
        if self._worker is not None:
            self._worker.join()
        with self._lock:
            if self._run is not None and not self._run.get('owner_closed'):
                if self._run['phase'] in ('running', 'draining'):
                    self._run.update(phase='closed', capture_incomplete=True)
                self._run['owner_closed'] = True
                self._run['unpersisted_on_close'] = [
                    {'capture_id': str(identity), 'frame': str(receipt['frame'])}
                    for identity, receipt in self._pending.items()]
                self._pending.clear()
                try:
                    self._manifest()
                except CapturePersistenceError:
                    pass

    @classmethod
    def _program(cls, arguments):
        path = Path(arguments.get('path', ''))
        loop = arguments.get('loop', False)
        if not path.is_absolute() or type(loop) is not bool:
            raise ValueError('Sequence requires an absolute file path and a boolean loop')
        try:
            def regular_opener(filename, flags):
                return os.open(filename, flags | getattr(os, 'O_NONBLOCK', 0))
            with open(path, 'rb', opener=regular_opener) as source:
                if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                    raise ValueError('Sequence path must identify a regular file')
                program = source.read(cls.MAX_PROGRAM + 1)
        except OSError as error:
            raise ValueError(f'Cannot read sequence program: {error}') from error
        if len(program) > cls.MAX_PROGRAM:
            raise ValueError('Sequence program exceeds 16 MiB')
        return program, loop

    def call(self, name, arguments):
        # Caller owns the same single-flight lock as compatibility pose edits.
        from simxr_control import Capability
        if self._closing.is_set():
            raise RuntimeError('Sequence owner closed')
        client, session = self._binding.client, self._binding.session
        if not client.hello['capabilities'] & Capability.POSE_SEQUENCES:
            raise RuntimeError('The selected runtime does not support acknowledged pose sequences')
        self._binding.observe()
        if name == 'run_pose_sequence':
            program, loop = self._program(arguments)
            if self._output is None:
                try:
                    self._directory.mkdir(parents=True, exist_ok=True)
                    self._output = Path(tempfile.mkdtemp(prefix='pose-sequence-', dir=self._directory))
                except OSError as error:
                    raise ValueError(f'Cannot create sequence output directory: {error}') from error
            if self._worker is None:
                self._worker = threading.Thread(target=self._work, name='simxr-sequence')
                try:
                    self._worker.start()
                except BaseException:
                    self._worker = None
                    raise
            uploaded = client.upload_sequence(session, program)
            accepted = client.start_sequence(session, uploaded, loop=loop)
            self._validate_status(accepted.get('document'), accepted.get('sequence_id'))
            previous = self._run
            self._run = {'sequence_id': accepted['sequence_id'], 'status': accepted['document'],
                         'phase': 'running', 'cursor': 0, 'captures_seen': 0, 'captures_saved': 0,
                         'capture_incomplete': False, 'history_gaps': 0}
            self._pending = {}
            # Replacement is already admitted. Retire only the old execution's
            # tickets; stopping the old ID must never cancel the new execution.
            self._wake.set()
            if previous is not None:
                try:
                    self._run['replacement'] = self._release_tickets(previous['sequence_id'])
                except Exception as error:
                    self._fail_connection(error)
                    raise
        else:
            if self._run is None:
                raise ValueError('No acknowledged sequence has been started by this owner')
            expected = arguments.get('sequence_id')
            if expected is not None and str(expected) != str(self._run['sequence_id']):
                raise ValueError('Sequence identity is no longer current')
            if name == 'stop_pose_sequence':
                self._run['status'] = client.stop_sequence(session, self._run['sequence_id'])['document']
            self._poll()
            self._wake.set()
        try:
            self._manifest()
        except CapturePersistenceError as error:
            self._fail_execution(error)
            raise
        return self._result()

    def _fail_connection(self, error):
        self._run.update(phase='failed', error=str(error), capture_incomplete=True)
        self._binding.invalidate(error)

    def _fail_execution(self, error):
        self._run.update(phase='failed', error=str(error), capture_incomplete=True)
        if self._binding.is_uncertain(error):
            self._binding.invalidate(error)
            return
        try:
            stopped = self._binding.client.stop_sequence(self._binding.session, self._run['sequence_id'])
            self._run['status'] = stopped['document']
            self._run['retired'] = self._release_tickets(self._run['sequence_id'])
            self._pending.clear()
        except Exception as cleanup:
            self._binding.invalidate(cleanup)

    @staticmethod
    def _validate_status(status, sequence):
        from simxr_control import ProtocolError
        if (type(sequence) is not int or not 0 < sequence < 2**64 or not isinstance(status, dict)
                or status.get('id') != sequence or type(status.get('active')) is not bool
                or status.get('result') not in ('queued', 'applied', 'cancelled', 'late', 'conflict', 'invalid', 'capacity', 'closed')
                or any(type(status.get(key)) is not int or not 0 <= status[key] < 2**64
                       for key in ('origin', 'last_frame'))):
            raise ProtocolError('Sequence acknowledgement omitted or changed its execution identity/state')

    def failed_status(self, arguments):
        if self._run is None or self._run['phase'] != 'failed':
            return None
        expected = arguments.get('sequence_id')
        if expected is not None and str(expected) != str(self._run['sequence_id']):
            raise ValueError('Sequence identity is no longer current')
        return self._result()

    def _result(self):
        result = {'ok': self._run['phase'] != 'failed' and not self._run['capture_incomplete'], 'transport': 'pose_sequences', 'session': str(self._binding.session.id),
                  'generation': str(self._binding.session.generation), **self._run,
                  'sequence_id': str(self._run['sequence_id']), 'cursor': str(self._run['cursor']),
                  'pending_captures': len(self._pending), 'recent_captures': list(self._records),
                  'evicted_results': self._evicted, 'directory': str(self._output)}
        status = dict(result['status'])
        for key in ('id', 'origin', 'last_frame'):
            status[key] = str(status[key])
        result['status'] = status
        return result

    def _manifest(self):
        encoded = json.dumps(self._result(), indent=2).encode('utf-8')
        if encoded != self._manifest_bytes:
            self._write(self._output / 'manifest.json', encoded)
            self._manifest_bytes = encoded

    @staticmethod
    def _write(path, content):
        temporary = path.with_suffix(path.suffix + '.tmp')
        try:
            try:
                temporary.write_bytes(content)
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
        except OSError as error:
            raise CapturePersistenceError(str(error)) from error

    def _record(self, receipt, *, png=None, error=None):
        entry = {**receipt, 'sequence_id': str(self._run['sequence_id']),
                 'frame': str(receipt['frame']), 'capture_id': str(receipt['capture_id'])}
        self._run['captures_seen'] += 1
        if error is not None:
            entry['error'] = str(error)
            self._run['capture_incomplete'] = True
        if png is not None:
            if len(png) > self.MAX_DISK:
                raise CapturePersistenceError('Sequence image exceeds its disk budget')
            path = self._output / f"sequence-{entry['sequence_id']}-frame-{entry['frame']}.png"
            self._write(path, png)
            entry.update(path=str(path), bytes=len(png))
            self._bytes += len(png)
            self._run['captures_saved'] += 1
        self._records.append(entry)
        while len(self._records) > self.MAX_RESULTS or self._bytes > self.MAX_DISK:
            old = self._records.popleft()
            if 'path' in old:
                try:
                    Path(old['path']).unlink(missing_ok=True)
                except OSError as error:
                    raise CapturePersistenceError(str(error)) from error
                self._bytes -= old['bytes']
            self._evicted += 1
        self._manifest()

    def _poll(self):
        client, session = self._binding.client, self._binding.session
        run = self._run
        page = client.sequence_status(session, run['sequence_id'], after=run['cursor'])['document']
        if page['status'] is None:
            raise RuntimeError('Sequence execution was replaced outside this owner')
        self._validate_status(page['status'], run['sequence_id'])
        run['status'] = page['status']
        if page['history_gap']:
            run['history_gaps'] += 1
            run['capture_incomplete'] = True
        run['cursor'] = page['next_frame']
        for receipt in page['captures']:
            if receipt['capture_id']:
                self._pending[receipt['capture_id']] = receipt
            else:
                self._record(receipt, error='Capture admission failed: ' + receipt['admission'])
        # Still-owned tickets remain discoverable after CPU history eviction.
        for receipt in page['held_captures']:
            self._pending.setdefault(receipt['capture_id'], {**receipt, 'admission': 'accepted'})
        if len(self._pending) > 64:
            raise RuntimeError('Sequence capture ownership exceeded its bounded capacity')
        if (run['phase'] != 'failed' and not run['status']['active'] and not page['more']
                and run['cursor'] >= run['status']['last_frame']):
            if self._pending:
                run['phase'] = 'draining'
            else:
                result = run['status']['result']
                run['phase'] = 'complete' if result == 'applied' else 'cancelled' if result == 'cancelled' else 'failed'

    def _release_tickets(self, sequence):
        client, session = self._binding.client, self._binding.session
        page = client.sequence_status(session, sequence)['document']
        for ticket in page['held_captures']:
            client.release_sequence_capture(session, ticket['capture_id'])
        return {'sequence_id': str(sequence), 'history_gap': page['history_gap'],
                'captures_not_persisted': [{'frame': str(t['frame']), 'capture_id': str(t['capture_id'])}
                                           for t in page['held_captures']]}

    def _wait(self, seconds):
        if self._closing.wait(seconds):
            raise InterruptedError('Sequence owner is closing')

    def _capture(self):
        from simxr_control import ControlError
        if not self._pending:
            return
        client, session = self._binding.client, self._binding.session
        identity = min(self._pending, key=lambda value: self._pending[value]['frame'])
        receipt = self._pending[identity]
        capture = client.capture(session, identity)
        phase = capture['phase']
        if phase in ('reserved', 'issued', 'begun'):
            return
        reader = None
        if phase in ('ready', 'gpu_pending'):
            try:
                reader = client.retain_image(session, identity)['image_reader_id']
            except ControlError as error:
                if error.code.name == 'BUSY':
                    return
                raise
        del self._pending[identity]
        png, failure = None, None
        try:
            client.release_sequence_capture(session, identity)
            receipt = {**receipt, 'phase': 'ready' if reader is not None else phase, 'result': capture['result']}
            if reader is None:
                failure = 'Sequence frame has no image: ' + phase
            else:
                info = client.image_info(reader)
                if info['frame'] != {'generation': session.generation, 'frame_id': receipt['frame']}:
                    from simxr_control import ProtocolError
                    raise ProtocolError('Sequence capture and image frame identities differ')
                image = read_stereo_image(client, reader, info['image_descriptor'], (0, 1),
                                          time.monotonic() + 5, self._wait, self.MAX_PIXELS)
                encoded = BytesIO()
                try:
                    image.save(encoded, format='PNG')
                except OSError as error:
                    raise CapturePersistenceError(str(error)) from error
                png = encoded.getvalue()
        except Exception as error:
            if self._binding.is_uncertain(error) or self._closing.is_set():
                raise
            failure = error
        finally:
            if reader is not None:
                client.release_image(reader)
        try:
            self._record(receipt, png=png, error=failure)
        except CapturePersistenceError:
            self._run['failed_capture'] = {'frame': str(receipt['frame']), 'capture_id': str(identity)}
            raise

    def _work(self):
        while not self._closing.is_set():
            self._wake.wait()
            self._wake.clear()
            if self._closing.is_set():
                return
            with self._lock:
                if self._run is None or self._run['phase'] not in ('running', 'draining'):
                    continue
                try:
                    self._poll()
                    self._capture()
                    self._poll()
                    self._manifest()
                except Exception as error:
                    if self._closing.is_set():
                        return
                    self._fail_execution(error)
                    try:
                        self._manifest()
                    except CapturePersistenceError:
                        pass
                active = self._run['phase'] in ('running', 'draining')
            if active and not self._closing.wait(.01):
                self._wake.set()
