"""Bounded recording collection and editable Luau replay export."""
from copy import deepcopy
import json

from .commands import ControlChannel, Scenario, decode_command, encode_command
from .protocol import MAX_PAYLOAD, ProtocolError

_MAX_TRANSACTIONS = 1024
_PAGE_SIZE = 16
_MAX_RECORDING_BYTES = 64 * 1024 * 1024
_MAX_COMMANDS = 64
_MAX_COMMAND_BYTES = 64 * 1024
_U64_MAX = 2**64 - 1
_PHASES = frozenset(('queued', 'applied', 'late', 'rescheduled', 'conflict',
                     'cancelled', 'invalid', 'capacity', 'closed', 'not_found', 'released'))
_IDENTITY_FIELDS = ('version', 'seed', 'origin_frame', 'origin_time', 'total',
                    'initial', 'initial_commands', 'initial_scenarios')


def _integer(value, name, minimum=0, maximum=_U64_MAX):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProtocolError('Invalid recording ' + name)
    return value


def _document_size(document):
    try:
        return len(json.dumps(document, ensure_ascii=False, allow_nan=False,
                              separators=(',', ':')).encode('utf-8'))
    except (TypeError, ValueError, UnicodeError, RecursionError) as error:
        raise ProtocolError('Malformed recording document') from error


def _commands(records):
    if not isinstance(records, list) or len(records) > _MAX_COMMANDS:
        raise ProtocolError('A recording command list contains at most 64 commands')
    commands = []
    byte_count = 0
    try:
        for record in records:
            if not isinstance(record, dict) or record.get('encoding') != 'simxr-command-v1-hex':
                raise ProtocolError('Unsupported recorded command encoding')
            data = record['data']
            if not isinstance(data, str) or len(data) > 2 * _MAX_COMMAND_BYTES or len(data) % 2:
                raise ProtocolError('Invalid recorded command length')
            encoded = bytes.fromhex(data)
            if not encoded or len(encoded) * 2 != len(data):
                raise ProtocolError('Recorded commands require hexadecimal bytes without whitespace')
            byte_count += len(encoded)
            if byte_count > _MAX_COMMAND_BYTES:
                raise ProtocolError('Recording command list exceeds 64 KiB')
            command = decode_command(record)
            # The v1 encoding is canonical. Refuse unknown/lost fields instead
            # of silently omitting them from an editable replay.
            if encode_command(command).value != encoded:
                raise ProtocolError('Recorded command cannot be exported without losing fields')
            commands.append(command)
    except (KeyError, TypeError, ValueError, OverflowError, IndexError) as error:
        raise ProtocolError('Malformed recorded command') from error
    return commands


def _validate_document(document):
    if not isinstance(document, dict) or type(document.get('version')) is not int or document['version'] not in (1, 2):
        raise ProtocolError('A v1 or v2 recording is required')
    if document.get('active') is not False:
        raise ProtocolError('Stop recording before saving an immutable replay')
    if document.get('overflow') is not False:
        raise ProtocolError('Recording overflowed or has no completeness flag')
    for name in ('seed', 'origin_frame'):
        _integer(document.get(name), name)
    _integer(document.get('origin_time'), 'origin_time', -(2**63), 2**63 - 1)
    _integer(document.get('total'), 'total', maximum=_MAX_TRANSACTIONS)
    if not isinstance(document.get('initial'), dict):
        raise ProtocolError('Recording initial configuration is missing')
    initial = _commands(document.get('initial_commands'))
    if any(command['kind'] in ('take_control', 'release_control', 'scenario') for command in initial):
        raise ProtocolError('Recording initial configuration cannot contain ownership or event commands')
    scenarios = document.get('initial_scenarios')
    if not isinstance(scenarios, list) or len(scenarios) > len(Scenario):
        raise ProtocolError('Recording initial scenario state is missing or exceeds its bound')
    seen = set()
    for scenario in scenarios:
        _integer(scenario, 'initial scenario', maximum=len(Scenario) - 1)
        if scenario in seen:
            raise ProtocolError('Duplicate recording initial scenario')
        seen.add(scenario)
    entries = document.get('transactions')
    if not isinstance(entries, list) or len(entries) > document['total']:
        raise ProtocolError('Invalid recording transaction count')
    for transaction in entries:
        if (not isinstance(transaction, dict) or not isinstance(transaction.get('phase'), str)
                or transaction['phase'] not in _PHASES):
            raise ProtocolError('Unknown recording transaction phase')
        released = transaction['phase'] == 'released'
        identity = _integer(transaction.get('transaction_id'), 'transaction_id', minimum=0 if released else 1)
        after = transaction.get('after_frame', False) if document['version'] == 1 else transaction.get('after_frame')
        if type(after) is not bool or (after and not released):
            raise ProtocolError('Invalid recorded release timing')
        commands = _commands(transaction.get('commands'))
        if released and (document['version'] != 2 or identity != 0 or not commands
                         or any(command['kind'] != 'release_control' for command in commands)):
            raise ProtocolError('Malformed recorded release event')
        frame = _integer(transaction.get('applied_frame'), 'applied_frame')
        if (transaction['phase'] == 'applied' or released) and (
                frame < document['origin_frame'] or (frame == document['origin_frame'] and not after)):
            raise ProtocolError('Applied transaction precedes the recording origin')
        if frame - document['origin_frame'] + after > _U64_MAX:
            raise ProtocolError('Release event exceeds replay frame range')


class _RecordingCollection:
    """Own copied pages only after their identity and progress are validated."""

    def __init__(self, session):
        self.session = session
        self.document = None
        self.offset = 0
        self.byte_count = 0

    def append(self, page):
        if not isinstance(page, dict):
            raise ProtocolError('Malformed recording page')
        session = _integer(page.get('session'), 'session', minimum=1)
        generation = _integer(page.get('generation'), 'generation')
        if session != self.session.id or generation != self.session.generation:
            raise ProtocolError('Recording session changed while it was being collected')
        if _integer(page.get('offset'), 'page offset', maximum=_MAX_TRANSACTIONS) != self.offset:
            raise ProtocolError('Invalid recording page offset')
        length = _integer(page.get('length'), 'page length', maximum=_PAGE_SIZE)
        if type(page.get('end')) is not bool:
            raise ProtocolError('Invalid recording page end marker')
        document = page.get('document')
        _validate_document(document)
        entries = document['transactions']
        next_offset = self.offset + len(entries)
        if length != len(entries) or next_offset > document['total']:
            raise ProtocolError('Invalid recording page length')
        complete = next_offset == document['total']
        if page['end'] != complete or (not complete and not entries):
            raise ProtocolError('Recording pagination is incomplete or made no progress')
        if self.document is not None and any(
                document[key] != self.document[key] for key in _IDENTITY_FIELDS):
            raise ProtocolError('Recording changed while it was being collected')
        page_bytes = _document_size(document)
        if self.byte_count + page_bytes > _MAX_RECORDING_BYTES:
            raise ProtocolError('Recording exceeds the 64 MiB collection budget')
        copied = deepcopy(document)
        if self.document is None:
            self.document = copied
        else:
            self.document['transactions'].extend(copied['transactions'])
        self.byte_count += page_bytes
        self.offset = next_offset
        return complete


def collect_recording(client, session):
    """Collect one stopped recording, bound to the supplied session generation.

    Pages retain their original normalized outcomes and initial configuration.
    A changed identity, incomplete end marker or exceeded bound fails explicitly.
    """
    collection = _RecordingCollection(session)
    while True:
        page = client.read_recording(session, offset=collection.offset, count=_PAGE_SIZE)
        if collection.append(page):
            return collection.document


def _quote(text):
    # Luau decimal escapes are exactly three digits. JSON's \\u00XX escapes
    # cannot be used verbatim in Luau string literals.
    pieces = ['"']
    for character in text:
        if character in ('"', '\\'):
            pieces.append('\\' + character)
        elif ord(character) < 32 or ord(character) == 127:
            pieces.append('\\' + str(ord(character)).zfill(3))
        else:
            pieces.append(character)
    pieces.append('"')
    return ''.join(pieces)


def _luau(value):
    if isinstance(value, dict):
        return '{' + ', '.join('[' + _quote(str(key)) + '] = ' + _luau(item)
                              for key, item in value.items()) + '}'
    if isinstance(value, (list, tuple)):
        return '{' + ', '.join(_luau(item) for item in value) + '}'
    if value is None:
        return 'nil'
    if type(value) is bool:
        return 'true' if value else 'false'
    if isinstance(value, str):
        return _quote(value)
    if type(value) is int and abs(value) > 2**53 - 1:
        return _quote(str(value))
    return json.dumps(value, allow_nan=False)


def to_luau(recording):
    """Export full editable commands using exact uint64 frame arithmetic.

    Replay controls simulator inputs, requiring ordinary exact Step support.
    It does not promise deterministic application/game state.
    """
    _validate_document(recording)
    if len(recording['transactions']) != recording['total']:
        raise ProtocolError('Collect all recording pages before exporting')
    if _document_size(recording) > _MAX_RECORDING_BYTES:
        raise ProtocolError('Recording exceeds the 64 MiB export input budget')
    if Scenario.SESSION_LOSS in recording['initial_scenarios']:
        raise ProtocolError('A recording begun after session loss has no replayable live-session origin')
    origins = [command for command in _commands(recording['initial_commands'])
               if command['kind'] == 'reference_spaces']
    if len(origins) != 1:
        raise ProtocolError('Recording requires one exact reference-space origin')
    seed = recording['seed']
    lines = [f"-- OpenXR Simulator recorded inputs, schema {recording['version']}. Edit the command tables below.",
             '-- Launch simxr_runner with --seed ' + str(seed),
             'assert(simxr.seed == ' + _quote(str(seed)) + ', "Recording seed differs")',
             'simxr.pause()',
             'local baseFrame = simxr.frame_add(simxr.state().frame_id or "0", "1")',
             'local function presentThrough(appliedFrame)',
             '    repeat',
             '        local capture, progress, sourceFrame = simxr.step_display()',
             '        if capture then simxr.release_capture(capture) end',
             '    until sourceFrame and not simxr.frame_before(sourceFrame, appliedFrame)',
             'end',
             'local function advance(target)',
             '    while simxr.frame_before(simxr.state().frame_id or "0", target) do',
             '        local capture = simxr.step()',
             '        local appliedFrame = simxr.state().frame_id or "0"',
             '        if capture then simxr.release_capture(capture) end',
             '        presentThrough(appliedFrame)',
             '    end',
             'end']
    initial = [{'kind': 'take_control', 'channel': channel.name.lower()} for channel in ControlChannel]
    initial += _commands(recording['initial_commands'])
    # The origin recenter flag describes a consumed event, not a fresh request.
    initial += [{'kind': 'scenario', 'scenario': scenario.name.lower(),
                 'enabled': scenario in recording['initial_scenarios']
                            and scenario not in (Scenario.RECENTER, Scenario.SESSION_LOSS)}
                for scenario in Scenario]
    if len(initial) > _MAX_COMMANDS:
        raise ProtocolError('Recording initialization exceeds 64 commands')
    lines += ['local initial = simxr.schedule({frame=baseFrame, commands=' + _luau(initial) + '})',
              'advance(baseFrame)', 'simxr.await_transaction(initial)']
    groups = {}
    for transaction in recording['transactions']:
        if transaction['phase'] not in ('applied', 'released'):
            lines.append('-- Observed transaction ' + str(transaction['transaction_id']) + ': ' + transaction['phase'])
            continue
        relative = transaction['applied_frame'] - recording['origin_frame'] + transaction.get('after_frame', False)
        groups.setdefault(relative, []).append(transaction)
    for relative, transactions in sorted(groups.items()):
        lines += ['do -- recorded frame +' + str(relative),
                  '    local target = simxr.frame_add(baseFrame, ' + _quote(str(relative)) + ')',
                  '    local pending = {}']
        for transaction in transactions:
            commands = _commands(transaction['commands'])
            lines.append('    table.insert(pending, simxr.schedule({frame=target, commands=' + _luau(commands) + '}))')
        lines += ['    advance(target)',
                  '    for _, id in pending do simxr.await_transaction(id) end', 'end']
    lines += ['simxr.release_inputs()', 'simxr.resume()', 'simxr.checkpoint("recording replay", true)']
    result = '\n'.join(lines) + '\n'
    if len(result.encode('utf-8')) > MAX_PAYLOAD:
        raise ProtocolError('Editable recording exceeds the 1 MiB runner source budget')
    return result
