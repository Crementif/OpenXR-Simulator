"""Typed synchronous local control API; unfinished exchanges require retry()."""
from dataclasses import dataclass
import threading
import time

from .connection import PacketConnection, ReceivedHandles
from .discovery import discover
from .errors import ControlError, ControlTimeout
from .protocol import (Capability, CommandKind, ErrorCode, Field, OpCode,
                       ProtocolError, Tag, TargetKind, MAX_PAYLOAD)


@dataclass(frozen=True)
class Session:
    id: int
    generation: int


@dataclass(frozen=True)
class Lease:
    session: Session
    id: int


class Client:
    def __init__(self, path=None, timeout=2.0, required=Capability.DEBUG_COMMANDS, name='Python SDK'):
        if path is None:
            candidates = discover()
            if len(candidates) != 1:
                raise ValueError('Select an explicit socket path when discovery finds zero or multiple runtimes')
            path = candidates[0]
        self.path = str(path)
        self._lock = threading.Lock()
        self._next_command = 1
        self._next_program = 1
        self._connection = PacketConnection(path, timeout)
        try:
            self.hello = self.request(OpCode.HELLO, [Field.text(Tag.CLIENT_NAME, name),
                                                   Field.u64(Tag.REQUIRED_CAPABILITIES, required)])
        except BaseException:
            self._connection.close()
            raise

    @property
    def timeout(self):
        return self._connection.timeout

    @timeout.setter
    def timeout(self, value):
        if not value > 0:
            raise ValueError('A positive control timeout is required')
        self._connection.timeout = value

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        self._connection.close()

    def request(self, opcode, fields=(), *, handles=()):
        return self._connection.request(opcode, fields, handles=handles)

    def retry(self):
        """Finish exactly the unfinished packet, without granting another admission."""
        return self._connection.retry()

    @staticmethod
    def _session(session):
        return [Field.u64(Tag.SESSION, session.id, True), Field.u64(Tag.GENERATION, session.generation, True)]

    @classmethod
    def _lease(cls, lease):
        return cls._session(lease.session) + [Field.u64(Tag.LEASE, lease.id, True)]

    def sessions(self):
        return [Session(item['session'], item['generation']) for item in self.request(OpCode.SESSIONS)['sessions']]

    def status(self, session):
        return self.request(OpCode.STATUS, self._session(session))

    def acquire(self, session):
        return Lease(session, self.request(OpCode.ACQUIRE, self._session(session))['lease'])

    def renew(self, lease):
        return self.request(OpCode.RENEW, self._lease(lease))

    def relinquish(self, lease):
        return self.request(OpCode.RELINQUISH, self._lease(lease))

    def submit(self, lease, command, *, command_id=None, lookahead=1, target=TargetKind.COMPOSED_EYES,
               layer=0, view=0, operation=None, capture=None, recording_capacity=2, recording_frames=0, additional_targets=(),
               advance_display=False, snapshot_budget_mib=None):
        if command_id is None:
            with self._lock:
                command_id = self._next_command
                self._next_command += 1
        else:
            with self._lock:
                self._next_command = max(self._next_command, command_id + 1)
        fields = self._lease(lease) + [Field.u64(Tag.COMMAND_ID, command_id, True),
            Field.u32(Tag.COMMAND_KIND, command, True), Field.u32(Tag.LOOKAHEAD, lookahead),
            Field.nested(Tag.TARGET, [Field.u32(Tag.CAPTURE_KIND, target, True),
                                      Field.u32(Tag.LAYER, layer), Field.u32(Tag.VIEW, view)])]
        if additional_targets:
            fields.append(Field.nested(Tag.ADDITIONAL_TARGETS, [
                Field.nested(index, [Field.u32(Tag.CAPTURE_KIND, kind, True),
                                    Field.u32(Tag.LAYER, layer), Field.u32(Tag.VIEW, view)])
                for index, (kind, layer, view) in enumerate(additional_targets, 1)], True))
        if command == CommandKind.START_IMAGE_RECORDING:
            fields.append(Field.u32(Tag.CAPACITY, recording_capacity, True))
            fields.append(Field.u64(Tag.LENGTH, recording_frames, True))
        if type(advance_display) is not bool:
            raise ProtocolError('Display advancement must be boolean')
        if advance_display:
            fields.append(Field.boolean(Tag.ADVANCE_DISPLAY, True))
        if snapshot_budget_mib is not None:
            if type(snapshot_budget_mib) is not int or not 128 <= snapshot_budget_mib <= 8192:
                raise ProtocolError('Snapshot budget must be in [128,8192] MiB')
            fields.append(Field.u32(Tag.SNAPSHOT_BUDGET_MIB, snapshot_budget_mib))
        if operation is not None:
            fields.append(Field.u64(Tag.OPERATION_ID, operation))
        if capture is not None:
            fields.append(Field.u64(Tag.CAPTURE_ID, capture))
        return self.request(OpCode.SUBMIT, fields)

    def operation(self, session, operation):
        return self.request(OpCode.OPERATION, self._session(session) + [Field.u64(Tag.OPERATION_ID, operation, True)])

    def capture(self, session, capture):
        return self.request(OpCode.CAPTURE, self._session(session) + [Field.u64(Tag.CAPTURE_ID, capture, True)])

    def inspect(self, session, capture):
        """Retain historical CPU state and its immutable feature revisions.

        Repeated inspection of the same frame reuses its connection-owned record.
        Call release_inspection() to relinquish this independent metadata reader.
        """
        return self.request(OpCode.INSPECT_FRAME, self._session(session) + [Field.u64(Tag.CAPTURE_ID, capture, True)])

    def revision(self, revision):
        return self.request(OpCode.FEATURE_REVISION, [Field.u64(Tag.REVISION_ID, revision, True)])

    def release_inspection(self, inspection):
        return self.request(OpCode.RELEASE_INSPECTION, [Field.u64(Tag.INSPECTION_ID, inspection, True)])

    def retain_image(self, session, capture, *, target=None, layer=0, view=0):
        fields = self._session(session) + [Field.u64(Tag.CAPTURE_ID, capture, True)]
        if target is not None:
            fields.append(Field.nested(Tag.TARGET, [Field.u32(Tag.CAPTURE_KIND, target, True),
                                                  Field.u32(Tag.LAYER, layer), Field.u32(Tag.VIEW, view)], True))
        return self.request(OpCode.RETAIN_IMAGE, fields)

    def image_info(self, image_reader):
        return self.request(OpCode.IMAGE_INFO, [Field.u64(Tag.IMAGE_READER_ID, image_reader, True)])

    def _history_fields(self, session, target):
        fields = self._session(session)
        if target is not None:
            fields.append(Field.nested(Tag.TARGET, [Field.u32(Tag.CAPTURE_KIND, target, True),
                                                  Field.u32(Tag.LAYER, 0), Field.u32(Tag.VIEW, 0)], True))
        return fields

    def begin_image_window(self, session, *, target=None):
        return self.request(OpCode.BEGIN_IMAGE_WINDOW, self._history_fields(session, target))

    def image_window(self, window):
        return self.request(OpCode.READ_IMAGE_WINDOW, [Field.u64(Tag.IMAGE_WINDOW_ID, window, True)])

    def retain_window_image(self, window, frame):
        return self.request(OpCode.RETAIN_WINDOW_IMAGE, [Field.u64(Tag.IMAGE_WINDOW_ID, window, True),
                                                       Field.u64(Tag.FRAME_ID, frame, True)])

    def release_image_window(self, window):
        return self.request(OpCode.RELEASE_IMAGE_WINDOW, [Field.u64(Tag.IMAGE_WINDOW_ID, window, True)])

    def image_history(self, session, *, count=10, target=None):
        if type(count) is not int or not 1 <= count <= 10:
            raise ProtocolError('Image history count must be in [1,10]')
        return self.request(OpCode.READ_IMAGE_HISTORY, self._history_fields(session, target) + [Field.u64(Tag.LENGTH, count)])

    def retain_historical_image(self, session, frame, *, target=None):
        return self.request(OpCode.RETAIN_HISTORICAL_IMAGE,
                            self._history_fields(session, target) + [Field.u64(Tag.FRAME_ID, frame, True)])

    def configure_previews(self, session, budget_mib, ram_mib=None):
        """Resize GPU history live; detached readers remain charged until released."""
        if not self.hello['capabilities'] & Capability.CONFIGURABLE_PREVIEWS:
            raise ProtocolError('Configurable preview history is unavailable')
        return self.request(OpCode.CONFIGURE_PREVIEWS, self._session(session) +
                            [Field.u64(Tag.PREVIEW_BUDGET, budget_mib * 1024 * 1024, True)] +
                            ([] if ram_mib is None else [Field.u64(Tag.PREVIEW_RAM_BUDGET, ram_mib * 1024 * 1024, True)]))

    def retain_preview(self, session, *, frame=None):
        """Retain the current display image, or an exact continuous-history frame."""
        fields = self._session(session)
        if frame is not None:
            if not self.hello['capabilities'] & Capability.CONTINUOUS_PREVIEWS:
                raise ProtocolError('Continuous preview history is unavailable')
            fields.append(Field.u64(Tag.FRAME_ID, frame, True))
        return self.request(OpCode.RETAIN_PREVIEW, fields)

    def share_image(self, image_reader, device_uuid, driver_uuid, consumer_fence_fd, *, completion_kind=0):
        device_uuid, driver_uuid = bytes(device_uuid), bytes(driver_uuid)
        if len(device_uuid) != 16 or len(driver_uuid) != 16:
            raise ProtocolError('External device and driver UUIDs require exactly 16 bytes')
        if completion_kind not in (0, 1):
            raise ProtocolError('Unknown image completion kind')
        fields = [Field.u64(Tag.IMAGE_READER_ID, image_reader, True),
                  Field.binary(Tag.DEVICE_UUID, device_uuid, True), Field.binary(Tag.DRIVER_UUID, driver_uuid, True)]
        if completion_kind:
            fields.append(Field.u32(Tag.COMPLETION_KIND, completion_kind, True))
        return self.request(OpCode.SHARE_IMAGE, fields, handles=(consumer_fence_fd,))

    def delivery_info(self, delivery):
        return self.request(OpCode.DELIVERY_INFO, [Field.u64(Tag.DELIVERY_ID, delivery, True)])

    def release_delivery(self, delivery):
        return self.request(OpCode.RELEASE_DELIVERY, [Field.u64(Tag.DELIVERY_ID, delivery, True)])

    def release_image(self, image_reader):
        return self.request(OpCode.RELEASE_IMAGE, [Field.u64(Tag.IMAGE_READER_ID, image_reader, True)])

    def start_export(self, image_reader, layer=None):
        fields = [Field.u64(Tag.IMAGE_READER_ID, image_reader, True)]
        if layer is not None:
            fields.append(Field.u32(Tag.LAYER, layer, True))
        return self.request(OpCode.START_EXPORT, fields)

    def start_video_export(self, session, first_frame, last_frame):
        """Pin an inclusive retained range and encode a silent stereo MP4 asynchronously."""
        if (type(first_frame) is not int or type(last_frame) is not int or
                not 0 < first_frame <= last_frame < 2**64 or last_frame - first_frame >= 1_000_000):
            raise ValueError('Select one to one million consecutive frames')
        return self.request(OpCode.START_VIDEO_EXPORT, self._session(session) + [
            Field.u64(Tag.FIRST_FRAME, first_frame, True), Field.u64(Tag.LAST_FRAME, last_frame, True)])

    def save_video(self, session, first_frame, last_frame, path, *, timeout=120.0, overwrite=False):
        """Save an inclusive clip atomically on this client's machine. No FFmpeg is needed."""
        from .video import save_video
        return save_video(self, session, first_frame, last_frame, path, timeout=timeout, overwrite=overwrite)

    def export_info(self, export):
        return self.request(OpCode.EXPORT_INFO, [Field.u64(Tag.EXPORT_ID, export, True)])

    def read_export(self, export, offset=0, length=MAX_PAYLOAD - 4096):
        if type(offset) is not int or not 0 <= offset < 2**64 or type(length) is not int or not 1 <= length <= MAX_PAYLOAD - 4096:
            raise ProtocolError('Export offset/length is outside the bounded read range')
        return self.request(OpCode.READ_EXPORT, [Field.u64(Tag.EXPORT_ID, export, True),
                            Field.u64(Tag.OFFSET, offset, True), Field.u64(Tag.LENGTH, length, True)])

    def release_export(self, export):
        return self.request(OpCode.RELEASE_EXPORT, [Field.u64(Tag.EXPORT_ID, export, True)])

    def acknowledge(self, lease, through):
        return self.request(OpCode.ACKNOWLEDGE, self._lease(lease) + [Field.u64(Tag.COMMAND_ID, through, True)])

    def wait(self, session, operation, timeout=5.0, interval=0.01):
        """Polls metadata only. Deadline never means GPU completion or cancellation."""
        deadline = time.monotonic() + timeout
        while True:
            record = self.operation(session, operation)
            if record['phase'] in ('complete', 'failed', 'cancelled', 'interrupted'):
                return record
            if record['phase'] not in ('queued', 'pending'):
                raise ProtocolError(f'Unsupported operation phase: {record["phase"]}')
            if time.monotonic() >= deadline:
                raise TimeoutError(f'Operation {operation} remains unfinished')
            time.sleep(interval)

    def schedule(self, session, commands, *, frame=None, time_ns=None, transaction_id=None,
                 source=1, late_policy=0):
        """Schedule one atomic input transaction at an exact future boundary.

        Source is fixed for a connection (manual=0, MCP=1, rig=2). Include an
        explicit take_control command before writing each channel. A queued
        response is admission, not application; poll simulation_status().
        """
        from .commands import encode_command, SourceKind, LatePolicy
        if (frame is None) == (time_ns is None):
            raise ProtocolError('Select exactly one frame or simulation time target')
        commands = tuple(commands)
        if not 1 <= len(commands) <= 64:
            raise ProtocolError('A transaction requires 1 through 64 commands')
        if transaction_id is None:
            with self._lock:
                transaction_id = getattr(self, '_next_transaction', 1)
                self._next_transaction = transaction_id + 1
        elif type(transaction_id) is not int or not 0 < transaction_id < 2**64:
            raise ProtocolError('A nonzero uint64 transaction_id is required')
        else:
            with self._lock:
                self._next_transaction = max(getattr(self, '_next_transaction', 1), transaction_id + 1)
        fields = self._session(session) + [Field.u64(Tag.TRANSACTION_ID, transaction_id, True),
            Field.u32(Tag.SOURCE_KIND, SourceKind(source), True), Field.u32(Tag.LATE_POLICY, LatePolicy(late_policy), True),
            Field.nested(Tag.COMMANDS, [encode_command(command, i + 1) for i, command in enumerate(commands)], True)]
        if frame is not None:
            if type(frame) is not int or not 0 < frame < 2**64:
                raise ProtocolError('A positive uint64 frame is required')
            fields.append(Field.u64(Tag.APPLY_FRAME, frame, True))
        else:
            if type(time_ns) is not int or not 0 <= time_ns < 2**63:
                raise ProtocolError('A nonnegative int64 simulation time is required')
            fields.append(Field.i64(Tag.APPLY_TIME, time_ns, True))
        return self.request(OpCode.SUBMIT_SIMULATION, fields)

    def simulation_status(self, session, transaction=None, *, source=1):
        fields = self._session(session) + [Field.u32(Tag.SOURCE_KIND, source)]
        if transaction is not None:
            fields.append(Field.u64(Tag.TRANSACTION_ID, transaction, True))
        return self.request(OpCode.SIMULATION_STATUS, fields)

    def cancel_simulation(self, session, transaction, *, source=1):
        return self.request(OpCode.CANCEL_SIMULATION, self._session(session) + [
            Field.u64(Tag.TRANSACTION_ID, transaction, True), Field.u32(Tag.SOURCE_KIND, source)])

    def release_control(self, session, *, source=1):
        """Cancel pending source work and release held inputs/ownership."""
        return self.request(OpCode.RELEASE_CONTROL, self._session(session) + [Field.u32(Tag.SOURCE_KIND, source)])

    def upload_sequence(self, session, program, *, program_id=None, source=1):
        """Upload JSON lines without executing them; one staged program per connection."""
        if isinstance(program, str):
            program = program.encode('utf-8')
        if not isinstance(program, bytes) or len(program) > 16 * 1024 * 1024:
            raise ProtocolError('Sequence programs must be JSON-lines bytes or text, at most 16 MiB')
        if not self.hello['capabilities'] & Capability.POSE_SEQUENCES:
            raise ProtocolError('Pose sequences are unavailable on this connection')
        with self._lock:
            if program_id is None:
                program_id = self._next_program
            if type(program_id) is not int or not 0 < program_id < 2**64:
                raise ProtocolError('A positive uint64 program ID is required')
            self._next_program = max(self._next_program, program_id + 1)
        chunk_size = 256 * 1024
        for offset in range(0, max(1, len(program)), chunk_size):
            chunk = program[offset:offset + chunk_size]
            response = self.request(OpCode.UPLOAD_SEQUENCE, self._session(session) + [
                Field.u32(Tag.SOURCE_KIND, source), Field.u64(Tag.PROGRAM_ID, program_id, True),
                Field.u64(Tag.OFFSET, offset, True), Field.u64(Tag.TOTAL_BYTES, len(program), True),
                Field(Tag.BYTES, chunk, True)])
            document = response['document']
            if (document['program_id'] != program_id or document['total'] != len(program) or
                    not offset + len(chunk) <= document['received'] <= len(program)):
                raise ProtocolError('Sequence upload acknowledgement changed identity or offset')
        return program_id

    def start_sequence(self, session, program_id, *, loop=False, source=1):
        """Start at the next ordinary frame boundary. Retrying a program does not restart it."""
        if type(loop) is not bool:
            raise ProtocolError('Sequence loop must be boolean')
        return self.request(OpCode.START_SEQUENCE, self._session(session) + [
            Field.u32(Tag.SOURCE_KIND, source), Field.u64(Tag.PROGRAM_ID, program_id, True), Field.boolean(Tag.LOOP, loop)])

    def sequence_status(self, session, sequence=None, *, after=0, count=32, source=1):
        if type(after) is not int or not 0 <= after < 2**64 or type(count) is not int or not 1 <= count <= 64:
            raise ProtocolError('Sequence pages require a uint64 cursor and count in [1,64]')
        fields = self._session(session) + [Field.u32(Tag.SOURCE_KIND, source),
                                         Field.u64(Tag.OFFSET, after), Field.u64(Tag.LENGTH, count)]
        if sequence is not None:
            fields.append(Field.u64(Tag.SEQUENCE_ID, sequence, True))
        return self.request(OpCode.READ_SEQUENCE, fields)

    def stop_sequence(self, session, sequence, *, source=1):
        return self.request(OpCode.STOP_SEQUENCE, self._session(session) + [
            Field.u32(Tag.SOURCE_KIND, source), Field.u64(Tag.SEQUENCE_ID, sequence, True)])

    def release_sequence_capture(self, session, capture):
        return self.request(OpCode.RELEASE_SEQUENCE_CAPTURE,
                            self._session(session) + [Field.u64(Tag.CAPTURE_ID, capture, True)])

    def start_recording(self, session, *, source=1):
        return self.request(OpCode.START_RECORDING, self._session(session) + [Field.u32(Tag.SOURCE_KIND, source)])

    def stop_recording(self, session, *, source=1):
        return self.request(OpCode.STOP_RECORDING, self._session(session) + [Field.u32(Tag.SOURCE_KIND, source)])

    def read_recording(self, session, *, offset=0, count=16):
        if type(offset) is not int or not 0 <= offset <= 1024 or type(count) is not int or not 0 <= count <= 16:
            raise ProtocolError('Recording pages require offset in [0,1024], count in [0,16]')
        return self.request(OpCode.READ_RECORDING, self._session(session) + [
            Field.u64(Tag.OFFSET, offset), Field.u64(Tag.LENGTH, count)])

    def wait_simulation(self, session, transaction, *, source=1, timeout=5.0, interval=0.01):
        deadline = time.monotonic() + timeout
        while True:
            result = self.simulation_status(session, transaction, source=source)
            phase = result.get('phase')
            if phase in ('applied', 'late', 'conflict', 'cancelled', 'invalid', 'capacity', 'closed', 'not_found'):
                return result
            if phase not in ('queued', 'rescheduled'):
                raise ProtocolError(f'Unknown simulation transaction phase: {phase!r}')
            if time.monotonic() >= deadline:
                raise TimeoutError(f'Simulation transaction {transaction} remains unfinished')
            time.sleep(interval)


    def frame_history(self, session, *, after=0, count=128):
        """Read submitted CPU frame metadata and applied inputs without a lease.

        Continue from the last returned frame_id. The response offset identifies
        the oldest retained frame, allowing callers to detect history gaps.
        """
        if type(after) is not int or not 0 <= after < 2**64 or type(count) is not int or not 1 <= count <= 128:
            raise ProtocolError('Frame history requires a uint64 cursor and count in [1,128]')
        return self.request(OpCode.READ_FRAME_HISTORY, self._session(session) + [
            Field.u64(Tag.OFFSET, after), Field.u64(Tag.LENGTH, count)])

    def projection_history(self, session, *, count=64):
        """Read bounded submitted projection metadata without a lease or GPU capture."""
        if type(count) is not int or not 1 <= count <= 64:
            raise ProtocolError('Projection history count must be in [1,64]')
        return self.request(OpCode.READ_PROJECTION_HISTORY,
                            self._session(session) + [Field.u64(Tag.LENGTH, count)])

    def diagnostics(self, session, *, after=0, count=128, frame=None):
        if type(after) is not int or not 0 <= after < 2**64 or type(count) is not int or not 0 <= count <= 256:
            raise ProtocolError('Diagnostic pages require a uint64 cursor and count in [0,256]')
        fields = self._session(session) + [Field.u64(Tag.OFFSET, after), Field.u64(Tag.LENGTH, count)]
        if frame is not None:
            fields.append(Field.u64(Tag.FRAME_ID, frame))
        return self.request(OpCode.READ_DIAGNOSTICS, fields)


    def game_status(self, session=None):
        return self.request(OpCode.GAME_STATUS, self._session(session) if session is not None else [])

    def game_command(self, session, game, kind, payload='', *, command_id=None):
        with self._lock:
            if command_id is None:
                command_id = getattr(self, '_next_game_command', 1)
            if type(command_id) is not int or not 0 < command_id < 2**64:
                raise ProtocolError('A positive uint64 game command ID is required')
            self._next_game_command = max(getattr(self, '_next_game_command', 1), command_id + 1)
        return self.request(OpCode.SUBMIT_GAME, (self._session(session) if session is not None else []) + [Field.u64(Tag.GAME_ID, game, True),
            Field.u64(Tag.COMMAND_ID, command_id, True), Field.u32(Tag.GAME_KIND, kind, True), Field.text(Tag.DETAIL, payload, True)])

    def game_operation(self, session, game, operation):
        return self.request(OpCode.GAME_OPERATION, (self._session(session) if session is not None else []) + [Field.u64(Tag.GAME_ID, game, True),
            Field.u64(Tag.OPERATION_ID, operation, True)])
