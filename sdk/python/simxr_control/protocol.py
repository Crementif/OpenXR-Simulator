"""Version 1 local control framing. Integers retain their complete wire width."""
from dataclasses import dataclass
from enum import IntEnum, IntFlag
import struct
import math

MAGIC = 0x32525853
MAJOR, MINOR = 1, 5
HEADER = struct.Struct('<IHHHHIQII')
TLV = struct.Struct('<HHI')
MAX_PAYLOAD, MAX_FIELDS = 1024 * 1024, 256
MAX_TRANSFER_HANDLES = 4


class OpCode(IntEnum):
    HELLO = 1
    SESSIONS = 2
    STATUS = 3
    ACQUIRE = 4
    RENEW = 5
    RELINQUISH = 6
    SUBMIT = 7
    OPERATION = 8
    ACKNOWLEDGE = 9
    CAPTURE = 10
    INSPECT_FRAME = 11
    FEATURE_REVISION = 12
    RELEASE_INSPECTION = 13
    RETAIN_IMAGE = 14
    IMAGE_INFO = 15
    SHARE_IMAGE = 16
    DELIVERY_INFO = 17
    RELEASE_DELIVERY = 18
    RELEASE_IMAGE = 19
    START_EXPORT = 20
    EXPORT_INFO = 21
    READ_EXPORT = 22
    RELEASE_EXPORT = 23
    RETAIN_PREVIEW = 24
    SUBMIT_SIMULATION = 25
    SIMULATION_STATUS = 26
    CANCEL_SIMULATION = 27
    RELEASE_CONTROL = 28
    START_RECORDING = 29
    STOP_RECORDING = 30
    READ_RECORDING = 31
    READ_DIAGNOSTICS = 32
    GAME_STATUS = 33
    SUBMIT_GAME = 34
    GAME_OPERATION = 35
    READ_PROJECTION_HISTORY = 36
    READ_IMAGE_HISTORY = 37
    RETAIN_HISTORICAL_IMAGE = 38
    BEGIN_IMAGE_WINDOW = 39
    READ_IMAGE_WINDOW = 40
    RETAIN_WINDOW_IMAGE = 41
    RELEASE_IMAGE_WINDOW = 42
    UPLOAD_SEQUENCE = 43
    START_SEQUENCE = 44
    READ_SEQUENCE = 45
    STOP_SEQUENCE = 46
    RELEASE_SEQUENCE_CAPTURE = 47


class ErrorCode(IntEnum):
    OK = 0
    INVALID = 1
    UNSUPPORTED_VERSION = 2
    UNSUPPORTED_COMMAND = 3
    NOT_NEGOTIATED = 4
    CAPACITY = 5
    NOT_FOUND = 6
    STALE_GENERATION = 7
    LEASE_REQUIRED = 8
    CONFLICT = 9
    BUSY = 10
    CLOSED = 11
    INTERNAL = 12
    UNSUPPORTED_FIELD = 13


class Capability(IntFlag):
    DEBUG_COMMANDS = 1 << 0
    SNAPSHOT_METADATA = 1 << 1
    PIPELINE_INSPECTION = 1 << 2
    COMPOSED_CAPTURE = 1 << 3
    LAYER_VIEW_CAPTURE = 1 << 4
    GPU_PREVIEW = 1 << 5
    IMAGE_EXPORT = 1 << 6
    FULL_FRAME_STATE = 1 << 7
    HISTORICAL_CPU_STATE = 1 << 8
    IMMUTABLE_REVISIONS = 1 << 9
    LIVE_PREVIEW = 1 << 10
    SIMULATION_COMMANDS = 1 << 11
    COMMAND_RECORDING = 1 << 12
    FRAME_DIAGNOSTICS = 1 << 13
    GAME_BRIDGE = 1 << 14
    DEPTH_CAPTURE = 1 << 15
    IMAGE_RECORDING = 1 << 16
    FILTERED_COMPOSITION = 1 << 17
    PROJECTION_HISTORY = 1 << 18
    IMAGE_HISTORY = 1 << 19
    IMAGE_WINDOW = 1 << 20
    QUAD_IMAGE_HISTORY = 1 << 21
    POSE_SEQUENCES = 1 << 22
    ROTATIONAL_REPROJECTION = 1 << 25


class CommandKind(IntEnum):
    PAUSE = 1
    STEP = 2
    STRICT_ADMISSION = 3
    DISPLAY_STEP = 4
    RESUME = 5
    CANCEL = 6
    RELEASE = 7
    START_IMAGE_RECORDING = 8
    STOP_IMAGE_RECORDING = 9


class TargetKind(IntEnum):
    COMPOSED_EYES = 1
    LAYER_VIEW = 2
    VIRTUAL_DISPLAY = 3
    LAYER_DEPTH = 4
    LAYER_DEPTH_LINEAR = 5
    PROJECTION_LAYERS = 6
    QUAD_LAYERS = 7


class Tag(IntEnum):
    ERROR = 1
    DETAIL = 2
    SESSION = 3
    GENERATION = 4
    LEASE = 5
    COMMAND_ID = 6
    OPERATION_ID = 7
    CAPTURE_ID = 8
    CLIENT_NAME = 10
    REQUIRED_CAPABILITIES = 11
    OPTIONAL_CAPABILITIES = 12
    MINOR = 13
    CAPABILITIES = 14
    PAYLOAD_LIMIT = 15
    SESSIONS = 20
    STATE = 21
    RUNNING = 22
    CLOSED = 23
    MANUAL = 24
    LOOKAHEAD = 25
    LEDGER_COUNT = 26
    PIPELINE = 30
    RETENTION = 31
    PRESENTATION = 32
    COMMAND_KIND = 41
    TARGET = 42
    CAPTURE_KIND = 50
    LAYER = 51
    VIEW = 52
    PHASE = 60
    PROGRESS = 61
    RESULT = 62
    RETENTION_RESULT = 63
    FRAME = 64
    CAPTURES = 65
    DIAGNOSTICS = 66
    OUTSTANDING = 67
    DISPLAY = 68
    PREDICTION = 70
    FRAME_ID = 71
    TICK = 72
    TIME = 73
    PERIOD = 74
    STAGE = 75
    SUBMISSION = 76
    OUTPUT_DISPOSITION = 80
    DROP_REASON = 81
    LAYER_COUNT = 82
    LEASE_REMAINING = 83
    DUPLICATE = 84
    WAIT_PENDING = 85
    PAUSED = 86
    PERMITS = 87
    WAITED = 88
    BEGUN = 89
    NEXT_ADMISSION = 90
    CAPACITY = 91
    HELD_SLOTS = 92
    RETIRING = 93
    LAST_SUBMITTED = 94
    COALESCED_CLEARS = 95
    KIND = 96
    SKIPPED_TICKS = 97
    IMAGE_AVAILABLE = 98
    SOURCE = 99
    REVISION_ID = 100
    INSPECTION_ID = 101
    DOCUMENT = 102
    REVISION_KIND = 103
    RELEASED = 104
    IMAGE_WINDOW_ID = 180
    PROGRAM_ID = 181
    SEQUENCE_ID = 182
    LOOP = 183
    IMAGE_TICK = 184
    PRESENTATION_POLICY = 185
    REPROJECTED = 186
    REPROJECTION_RESULT = 187
    REPROJECTION_SUBMISSION_TIME = 188
    REPROJECTION_COMPLETION_LATENCY = 189
    DISPLAY_HEAD = 190
    IMAGE_READER_ID = 105
    DELIVERY_ID = 106
    POOL_GENERATION = 107
    SHARED_IMAGE_ID = 108
    EXPORT_ID = 109
    DEVICE_UUID = 110
    DRIVER_UUID = 111
    IMAGE_DESCRIPTOR = 112
    FORMAT = 115
    WIDTH = 116
    HEIGHT = 117
    DEPTH = 118
    ARRAY_LAYERS = 119
    MIP_LEVELS = 120
    SAMPLES = 121
    USAGE = 122
    TILING = 123
    IMAGE_TYPE = 124
    IMAGE_FLAGS = 125
    ALLOCATION_SIZE = 126
    MEMORY_TYPE_INDEX = 127
    DEDICATED = 128
    PRODUCER_QUEUE_FAMILY = 129
    LAYOUT = 130
    PRODUCER_PHASE = 131
    CONSUMER_PHASE = 132
    OFFSET = 133
    LENGTH = 134
    BYTES = 135
    TOTAL_BYTES = 136
    END = 137
    HANDLE_KIND = 138
    HANDLE_COUNT = 139
    COMMANDS = 140
    TRANSACTION_ID = 141
    APPLY_FRAME = 142
    APPLY_TIME = 143
    LATE_POLICY = 144
    SOURCE_KIND = 145
    TAKE_CONTROL = 146
    CHANNEL = 147
    VALUES = 148
    ENABLED = 149
    POSE = 150
    POSITION = 151
    ORIENTATION = 152
    CONTROLS = 153
    PROPORTIONS = 154
    SCENARIO = 155
    SEED = 156
    OVERFLOW = 158
    RECORDING = 159
    OWNERSHIP = 160
    TRANSACTIONS = 161
    REVISION = 162
    APPLIED_FRAME = 163
    APPLIED_TIME = 164
    RESCHEDULED = 165
    GAME_ID = 166
    GAME_KIND = 167
    IMAGE_RECORDING = 168
    TOTAL_FRAMES = 169


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class Field:
    tag: int
    value: bytes
    required: bool = False

    @classmethod
    def u64(cls, tag, value, required=False):
        return cls(tag, struct.pack('<Q', value), required)

    @classmethod
    def u32(cls, tag, value, required=False):
        return cls(tag, struct.pack('<I', value), required)

    @classmethod
    def i64(cls, tag, value, required=False):
        return cls(tag, struct.pack('<q', value), required)

    @classmethod
    def f32(cls, tag, value, required=False):
        return cls(tag, struct.pack('<f', value), required)

    @classmethod
    def boolean(cls, tag, value, required=False):
        if type(value) is not bool:
            raise ProtocolError('Boolean value required')
        return cls(tag, bytes([value]), required)

    @classmethod
    def text(cls, tag, value, required=False):
        if '\0' in value:
            raise ProtocolError('Text cannot contain NUL')
        return cls(tag, value.encode('utf-8', errors='strict'), required)

    @classmethod
    def binary(cls, tag, value, required=False):
        return cls(tag, bytes(value), required)

    @classmethod
    def nested(cls, tag, fields, required=False):
        return cls(tag, encode_fields(fields), required)

    def _unpack(self, shape):
        try:
            return struct.unpack(shape, self.value)[0]
        except struct.error as error:
            raise ProtocolError(f'Invalid field {self.tag} width') from error

    def as_u64(self):
        return self._unpack('<Q')

    def as_u32(self):
        return self._unpack('<I')

    def as_i64(self):
        return self._unpack('<q')

    def as_f32(self):
        return self._unpack('<f')

    def as_bool(self):
        if self.value not in (b'\0', b'\1'):
            raise ProtocolError(f'Invalid boolean field {self.tag}')
        return self.value == b'\1'

    def as_text(self):
        if b'\0' in self.value:
            raise ProtocolError('Text cannot contain NUL')
        try:
            return self.value.decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            raise ProtocolError('Invalid UTF-8') from error


def encode_fields(fields):
    fields = tuple(fields)
    if len(fields) > MAX_FIELDS or len({field.tag for field in fields}) != len(fields):
        raise ProtocolError('Too many or duplicate fields')
    result = bytearray()
    for field in fields:
        if not 0 < field.tag <= 65535 or type(field.required) is not bool:
            raise ProtocolError('Invalid field tag/flags')
        if len(field.value) > MAX_PAYLOAD - TLV.size - len(result):
            raise ProtocolError('Payload capacity exceeded')
        result += TLV.pack(field.tag, int(field.required), len(field.value))
        result += field.value
    return bytes(result)


def decode_fields(payload):
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError('Payload capacity exceeded')
    fields, offset = {}, 0
    while offset < len(payload):
        if len(payload) - offset < TLV.size or len(fields) >= MAX_FIELDS:
            raise ProtocolError('Invalid field count/length')
        tag, flags, size = TLV.unpack_from(payload, offset)
        offset += TLV.size
        if not tag or tag in fields or flags > 1 or size > len(payload) - offset:
            raise ProtocolError('Invalid field tag/flags/length')
        fields[tag] = Field(tag, bytes(payload[offset:offset + size]), bool(flags))
        offset += size
    return fields


@dataclass(frozen=True)
class Message:
    opcode: int
    request_id: int
    fields: tuple = ()
    flags: int = 0
    major: int = MAJOR
    minor: int = MINOR

    def encode(self):
        if self.major != MAJOR or self.flags not in (0, 1) or not 0 < self.request_id < 2**64:
            raise ProtocolError('Invalid envelope')
        payload = encode_fields(self.fields)
        return HEADER.pack(MAGIC, self.major, self.minor, self.opcode, self.flags, 0,
                           self.request_id, len(payload), 0) + payload

    @staticmethod
    def size(header):
        if len(header) != HEADER.size:
            raise ProtocolError('Expected complete header')
        magic, major, minor, opcode, flags, reserved, request, size, reserved2 = HEADER.unpack(header)
        if magic != MAGIC or major != MAJOR or flags > 1 or reserved or reserved2 or not request:
            raise ProtocolError('Invalid or unsupported envelope')
        if size > MAX_PAYLOAD:
            raise ProtocolError('Payload capacity exceeded')
        return HEADER.size + size

    @classmethod
    def decode(cls, packet):
        total = cls.size(packet[:HEADER.size])
        if len(packet) != total:
            raise ProtocolError('Incomplete message or trailing bytes')
        _, major, minor, opcode, flags, _, request, _, _ = HEADER.unpack_from(packet)
        return cls(opcode, request, tuple(decode_fields(packet[HEADER.size:]).values()), flags, major, minor)


_TEXT = {Tag.PRESENTATION_POLICY, Tag.DETAIL, Tag.CLIENT_NAME, Tag.PHASE, Tag.PROGRESS, Tag.STAGE, Tag.OUTPUT_DISPOSITION, Tag.DROP_REASON, Tag.KIND,
         Tag.RETENTION_RESULT, Tag.REVISION_KIND, Tag.PRODUCER_PHASE, Tag.CONSUMER_PHASE}
_BOOL = {Tag.REPROJECTED, Tag.RUNNING, Tag.CLOSED, Tag.MANUAL, Tag.DUPLICATE, Tag.WAIT_PENDING, Tag.PAUSED, Tag.IMAGE_AVAILABLE, Tag.RELEASED, Tag.DEDICATED, Tag.END, Tag.TAKE_CONTROL, Tag.ENABLED, Tag.OVERFLOW, Tag.RECORDING, Tag.RESCHEDULED, Tag.LOOP}
_U32 = {Tag.ERROR, Tag.MINOR, Tag.PAYLOAD_LIMIT, Tag.LOOKAHEAD, Tag.COMMAND_KIND, Tag.CAPTURE_KIND,
        Tag.LAYER, Tag.VIEW, Tag.LAYER_COUNT, Tag.PERMITS, Tag.FORMAT, Tag.WIDTH, Tag.HEIGHT,
        Tag.DEPTH, Tag.ARRAY_LAYERS, Tag.MIP_LEVELS, Tag.SAMPLES, Tag.USAGE, Tag.TILING, Tag.IMAGE_TYPE,
        Tag.IMAGE_FLAGS, Tag.MEMORY_TYPE_INDEX, Tag.PRODUCER_QUEUE_FAMILY, Tag.LAYOUT, Tag.HANDLE_KIND, Tag.HANDLE_COUNT, Tag.LATE_POLICY, Tag.SOURCE_KIND, Tag.CHANNEL, Tag.SCENARIO, Tag.GAME_KIND}
_I64 = {Tag.REPROJECTION_RESULT, Tag.REPROJECTION_SUBMISSION_TIME, Tag.REPROJECTION_COMPLETION_LATENCY, Tag.STATE, Tag.RESULT, Tag.TIME, Tag.PERIOD, Tag.LEASE_REMAINING, Tag.APPLY_TIME, Tag.APPLIED_TIME}
_NESTED = {Tag.IMAGE_RECORDING, Tag.PIPELINE, Tag.RETENTION, Tag.PRESENTATION, Tag.TARGET, Tag.FRAME, Tag.DISPLAY,
           Tag.PREDICTION, Tag.SUBMISSION, Tag.WAITED, Tag.BEGUN, Tag.SOURCE, Tag.IMAGE_DESCRIPTOR}
_LIST = {Tag.SESSIONS, Tag.CAPTURES, Tag.DIAGNOSTICS, Tag.OUTSTANDING}


def to_dict(fields):
    """Decode known optional records and skip unknown optional fields."""
    result = {}
    for field in fields:
        try:
            tag = Tag(field.tag)
        except ValueError:
            if field.required:
                raise ProtocolError(f'Unsupported required response field {field.tag}')
            continue
        if tag in (Tag.DEVICE_UUID, Tag.DRIVER_UUID):
            if len(field.value) != 16:
                raise ProtocolError('External device and driver UUIDs must contain exactly 16 bytes')
            value = field.value
        elif tag == Tag.BYTES:
            value = field.value
        elif tag in (Tag.DOCUMENT, Tag.DISPLAY_HEAD):
            value = decode_inspection(field.value)
        elif tag in _TEXT:
            value = field.as_text()
        elif tag in _BOOL:
            value = field.as_bool()
        elif tag in _U32:
            value = field.as_u32()
        elif tag in _I64:
            value = field.as_i64()
        elif tag in _NESTED:
            value = to_dict(decode_fields(field.value).values())
        elif tag in _LIST:
            entries = decode_fields(field.value)
            if tuple(entries) != tuple(range(1, len(entries) + 1)):
                raise ProtocolError('Noncontiguous list entry tags')
            value = [entry.as_u64() if tag in (Tag.CAPTURES, Tag.DIAGNOSTICS)
                     else to_dict(decode_fields(entry.value).values()) for entry in entries.values()]
        else:
            value = field.as_u64()
        result[tag.name.lower()] = value
    return result


def decode_inspection(payload):
    """Decode the bounded v1 inspection tree. Revision references stay explicit.

    Non-finite float32 data becomes the strings NaN, +Infinity or -Infinity so
    downstream JSON stays standards compliant without silently losing a value.
    """
    remaining = [65536]

    def sequence(data):
        fields = decode_fields(data)
        if tuple(fields) != tuple(range(1, len(fields) + 1)):
            raise ProtocolError('Noncontiguous inspection child tags')
        return tuple(fields.values())

    def node(data, depth, expected_name=None):
        remaining[0] -= 1
        if depth > 32 or remaining[0] < 0:
            raise ProtocolError('Inspection depth/node capacity exceeded')
        fields = decode_fields(data)
        if not {1, 2, 3} <= fields.keys() or any(field.required and tag not in (1, 2, 3)
                                               for tag, field in fields.items()):
            raise ProtocolError('Invalid or unsupported inspection node fields')
        name, kind, value = fields[1].as_text(), fields[2].as_u32(), fields[3]
        if expected_name is not None and name != expected_name:
            raise ProtocolError('Unexpected inspection node name')
        if kind == 0:
            if value.value:
                raise ProtocolError('Nonempty null inspection value')
            result = None
        elif kind == 1:
            result = value.as_bool()
        elif kind == 2:
            result = value.as_u64()
        elif kind == 3:
            result = value.as_i64()
        elif kind == 4:
            result = value.as_f32()
            if not math.isfinite(result):
                result = 'NaN' if math.isnan(result) else '+Infinity' if result > 0 else '-Infinity'
        elif kind == 5:
            result = value.as_text()
        elif kind == 6:
            result = {}
            for child in sequence(value.value):
                child_name, child_value = node(child.value, depth + 1)
                if not child_name or child_name in result:
                    raise ProtocolError('Empty or duplicate inspection object name')
                result[child_name] = child_value
        elif kind == 7:
            result = []
            pages = sequence(value.value)
            for index, page in enumerate(pages):
                children = sequence(page.value)
                if not children or (index + 1 < len(pages) and len(children) != MAX_FIELDS):
                    raise ProtocolError('Invalid inspection array page length')
                for child in children:
                    result.append(node(child.value, depth + 1, '')[1])
        elif kind == 8:
            revision = value.as_u64()
            if not revision:
                raise ProtocolError('Zero inspection revision ID')
            result = {'revision_id': revision}
        else:
            raise ProtocolError('Unsupported inspection value type')
        return name, result

    return node(payload, 0, '')[1]
