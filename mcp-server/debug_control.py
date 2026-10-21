"""MCP-neutral adapter for the versioned local debugger protocol.

The legacy MCP process may import this module without an installed SDK or MCP
package. It loads the SDK only when a control tool is called. Capture results are
CPU metadata. Image export requires its separate explicit tools; native GPU
FD sharing stays in native clients, and no tool falls back to legacy files.
"""
import importlib
import base64
import math
import re
from pathlib import Path
import sys
import threading
import uuid


def _sdk():
    try:
        return importlib.import_module('simxr_control')
    except ModuleNotFoundError as error:
        if error.name != 'simxr_control':
            raise
        checkout = Path(__file__).resolve().parents[1] / 'sdk' / 'python'
        if not (checkout / 'simxr_control' / '__init__.py').is_file():
            raise
        location = str(checkout)
        if location not in sys.path:
            sys.path.insert(0, location)
        return importlib.import_module('simxr_control')


_MAX_U64 = 2**64 - 1
_SAFE_INTEGER = 2**53 - 1
_ID = {'anyOf': [{'type': 'string', 'pattern': '^[1-9][0-9]{0,19}$', 'maxLength': 20},
                 {'type': 'integer', 'minimum': 1, 'maximum': _SAFE_INTEGER}],
       'description': 'Opaque uint64 identifier. Use its returned canonical decimal string; numeric input must be IEEE754-safe.'}
_CONNECTION = {'type': 'string', 'minLength': 1}
_GENERATION = {'anyOf': [{'type': 'string', 'pattern': '^(0|[1-9][0-9]{0,19})$', 'maxLength': 20},
                         {'type': 'integer', 'minimum': 0, 'maximum': _SAFE_INTEGER}],
               'description': 'Session generation as a canonical decimal string; zero identifies a session before its first Begin.'}
_SESSION = {'connection': _CONNECTION, 'session': _ID, 'generation': _GENERATION}
_GAME = {'connection': _CONNECTION, 'session': _ID, 'generation': _GENERATION}
_LEASE = {**_SESSION, 'lease': _ID}
_COMMAND = {**_LEASE, 'command_id': _ID}
_TARGET = {'target': {'type': 'string', 'enum': ['composed_eyes', 'layer_view', 'layer_depth', 'layer_depth_linear'], 'default': 'composed_eyes'},
           'layer': {'type': 'integer', 'minimum': 0, 'maximum': 2**32 - 1, 'default': 0},
           'view': {'type': 'integer', 'minimum': 0, 'maximum': 2**32 - 1, 'default': 0},
           'lookahead': {'type': 'integer', 'minimum': 1, 'maximum': 4, 'default': 1}}


def _tool(name, description, properties, required=None):
    return {'name': name, 'description': description,
            'inputSchema': {'type': 'object', 'properties': properties,
                            'required': list(properties) if required is None else required,
                            'additionalProperties': False}}


TOOL_DEFINITIONS = [
    _tool('debug_connect', 'Connect to a versioned local runtime and return its hello and opaque session identities.',
          {'path': {'type': 'string', 'minLength': 1},
           'timeout': {'type': 'number', 'exclusiveMinimum': 0, 'maximum': 30, 'default': 2.0},
           'name': {'type': 'string', 'default': 'MCP debugger'}}, []),
    _tool('debug_disconnect', 'Disconnect this client; an owned control lease recovers asynchronously.',
          {'connection': _CONNECTION}),
    _tool('debug_status', 'Inspect CPU control, pipeline and retention state without advancing a frame.', _SESSION),
    _tool('debug_acquire', 'Acquire the single debugger lease for this exact session generation.', _SESSION),
    _tool('debug_renew', 'Renew this connection\'s lease for the exact session generation.', _LEASE),
    _tool('debug_relinquish', 'Relinquish the lease and request asynchronous recovery.', _LEASE),
    _tool('debug_pause', 'Admit an asynchronous pause. Poll the operation to learn whether its boundary settled.', _COMMAND),
    _tool('debug_step', 'Inspect the next submitted frame. Reserve capture before any bounded lookahead admission.',
          {**_COMMAND, **_TARGET}, list(_COMMAND)),
    _tool('debug_strict_admission', 'Reserve one exact future capture and grant one application-frame admission.',
          {**_COMMAND, **_TARGET}, list(_COMMAND)),
    _tool('debug_display_step', 'Advance one virtual display tick while paused; this does not issue an XR frame.', _COMMAND),
    _tool('debug_resume', 'Resume the original execution mode while retaining explicitly captured snapshots.', _COMMAND),
    _tool('debug_cancel', 'Cancel unused permits for an operation; issued XR work is not cancelled.',
          {**_COMMAND, 'operation': _ID}),
    _tool('debug_release', 'Release one logical capture. Existing GPU/readers retain safe resource ownership.',
          {**_COMMAND, 'capture': _ID}),
    _tool('debug_operation', 'Poll an admitted operation; polling never issues another admission.',
          {**_SESSION, 'operation': _ID}),
    _tool('debug_capture', 'Read exact capture CPU metadata only. Use separate explicit image-export tools for pixels.',
          {**_SESSION, 'capture': _ID}),
    _tool('debug_inspect', 'Retain submitted historical CPU input, tracking, actions, haptics and layers. Larger definitions use immutable revision references; unavailable sections are explicit.',
          {**_SESSION, 'capture': _ID}),
    _tool('debug_revision', 'Read a connection-owned immutable feature revision while its inspection is retained.',
          {'connection': _CONNECTION, 'revision': _ID}),
    _tool('debug_release_inspection', 'Release an independent CPU inspection reader and its unused revision references.',
          {'connection': _CONNECTION, 'inspection': _ID}),
    _tool('debug_retain_image', 'Retain an exact captured image, optionally selecting another already retained target from the same frame; never admits work or reads pixels.',
          {**_SESSION, 'capture': _ID,
           'target': {key: value for key, value in _TARGET['target'].items() if key != 'default'},
           'layer': _TARGET['layer'], 'view': _TARGET['view']}, [*_SESSION, 'capture']),
    _tool('debug_retain_preview', 'Retain the currently selected completed virtual-display image, or report its actual clear; no admission or pixel readback.',
          _SESSION),
    _tool('debug_image_info', 'Inspect retained image metadata and capabilities without reading pixels.',
          {'connection': _CONNECTION, 'image': _ID}),
    _tool('debug_release_image', 'Release an image reader; independent deliveries and exports remain retained.',
          {'connection': _CONNECTION, 'image': _ID}),
    _tool('debug_delivery_info', 'Inspect an existing connection-owned native delivery without waiting on or advancing GPU work.',
          {'connection': _CONNECTION, 'delivery': _ID}),
    _tool('debug_release_delivery', 'Release a logical native delivery; backend ownership still follows actual completion.',
          {'connection': _CONNECTION, 'delivery': _ID}),
    _tool('debug_start_export', 'Explicitly request a bounded GPU-to-CPU image export. Poll its ticket; creation does not return pixels.',
          {'connection': _CONNECTION, 'image': _ID,
           'layer': {'type': 'integer', 'minimum': 0, 'maximum': 2**32 - 1}}, ['connection', 'image']),
    _tool('debug_export_info', 'Poll explicit image-export metadata. Pending never means complete.',
          {'connection': _CONNECTION, 'export': _ID}),
    _tool('debug_read_export', 'Explicitly read a bounded completed export chunk. The bytes field contains base64 data with encoding metadata.',
          {'connection': _CONNECTION, 'export': _ID,
           'offset': {**_GENERATION, 'description': 'Zero-based byte offset as a canonical decimal uint64 string or safe integer.'},
           'length': {'type': 'integer', 'minimum': 1, 'maximum': 1024 * 1024 - 4096}}, ['connection', 'export']),
    _tool('debug_release_export', 'Release one explicit CPU export ticket independently of the image reader.',
          {'connection': _CONNECTION, 'export': _ID}),
    _tool('debug_acknowledge', 'Retire completed command IDs through this value; retired retries cannot execute again.',
          {**_LEASE, 'through': _ID}),
    _tool('simulation_schedule', 'Schedule an atomic simulator-input transaction at an exact frame or simulation time. Explicit take_control commands establish ownership; poll the transaction for actual application.',
          {**_SESSION, 'transaction_id': _ID, 'frame': _ID,
           'time_ns': {'type': 'string', 'pattern': '^(0|[1-9][0-9]{0,18})$', 'maxLength': 19},
           'commands': {'type': 'array', 'minItems': 1, 'maxItems': 64, 'items': {'type': 'object'}},
           'late_policy': {'type': 'string', 'enum': ['reject', 'reschedule_next'], 'default': 'reject'}},
          [*_SESSION, 'commands', 'transaction_id']),
    _tool('simulation_status', 'Read applied simulator state, ownership and bounded transaction outcomes without changing application input or action-sync state.',
          {**_SESSION, 'transaction_id': _ID}, list(_SESSION)),
    _tool('simulation_cancel', 'Cancel an unapplied transaction owned by this connection.', {**_SESSION, 'transaction_id': _ID}),
    _tool('simulation_release_control', 'Cancel pending simulator commands and release this source\'s held inputs and ownership.', _SESSION),
    _tool('simulation_start_recording', 'Start bounded recording of acknowledged simulator transactions and initial configuration. Another recording owner is rejected.', _SESSION),
    _tool('simulation_stop_recording', 'Stop this connection\'s recording without releasing its independent inputs.', _SESSION),
    _tool('simulation_read_recording', 'Read a bounded page of acknowledged normalized simulator commands. Overflow and rejected outcomes remain explicit.',
          {**_SESSION, 'offset': {**_GENERATION, 'description': 'Recording transaction offset.'},
           'count': {'type': 'integer', 'minimum': 0, 'maximum': 16}}, list(_SESSION)),
    _tool('simulation_diagnostics', 'Read bounded observed frame milestones and diagnostics. Performance misses are distinct from API errors; reads do not advance frames.',
          {**_SESSION, 'after': {**_GENERATION, 'description': 'Diagnostic sequence cursor.'},
           'count': {'type': 'integer', 'minimum': 0, 'maximum': 256}, 'frame': _ID}, list(_SESSION)),
    _tool('game_bridge_status', 'Inspect the optional cooperating game bridge, actual supported reset/step/custom capabilities and latest bounded telemetry.', _GAME, ['connection']),
    _tool('game_bridge_command', 'Queue an explicitly supported game command. A queued response does not mean it executed; the game acknowledges it at its own safe update point.',
          {**_GAME, 'game': _ID, 'command_id': _ID, 'kind': {'type': 'integer', 'minimum': 1, 'maximum': 3},
           'payload': {'type': 'string', 'default': ''}}, ['connection', 'game', 'command_id', 'kind']),
    _tool('game_bridge_operation', 'Read actual game-side acknowledgement. This bridge is not required for XR input or capture.',
          {**_GAME, 'game': _ID, 'operation': _ID}, ['connection', 'game', 'operation']),
    _tool('debug_retry' , 'Finish this connection\'s unfinished SDK exchange without sending another command.',
          {'connection': _CONNECTION}),
]
_SCHEMAS = {tool['name']: tool['inputSchema'] for tool in TOOL_DEFINITIONS}
_COMMAND_NAMES = {'debug_pause': 'PAUSE', 'debug_step': 'STEP', 'debug_strict_admission': 'STRICT_ADMISSION',
                  'debug_display_step': 'DISPLAY_STEP', 'debug_resume': 'RESUME', 'debug_cancel': 'CANCEL',
                  'debug_release': 'RELEASE'}


def _validate(name, args):
    if name not in _SCHEMAS:
        raise LookupError('Unknown debugger tool')
    if not isinstance(args, dict):
        raise ValueError('Tool arguments must be an object')
    schema = _SCHEMAS[name]
    if set(args) - set(schema['properties']):
        raise ValueError('Unknown debugger argument')
    missing = set(schema['required']) - set(args)
    if missing:
        raise ValueError('Missing required arguments: ' + ', '.join(sorted(missing)))
    normalized = dict(args)
    for key, value in args.items():
        rule = schema['properties'][key]
        if 'anyOf' in rule:
            minimum = 0 if key in ('generation', 'offset', 'after') else 1
            if isinstance(value, str):
                if len(value) > 20 or re.fullmatch(r'0|[1-9][0-9]*', value) is None:
                    raise ValueError(f'{key} must be a canonical decimal uint64 string')
                parsed = int(value)
                if not minimum <= parsed <= _MAX_U64:
                    raise ValueError(f'{key} is outside its uint64 range')
            elif type(value) is int and minimum <= value <= _SAFE_INTEGER:
                parsed = value
            else:
                raise ValueError(f'{key} requires a decimal string or an IEEE754-safe integer')
            normalized[key] = parsed
            continue
        kind = rule['type']
        if kind == 'integer':
            if type(value) is not int or not rule['minimum'] <= value <= rule['maximum']:
                raise ValueError(f'{key} must be an integer in [{rule["minimum"]}, {rule["maximum"]}]')
        elif kind == 'number':
            if type(value) not in (int, float) or value <= 0 or value > rule['maximum'] or not math.isfinite(value):
                raise ValueError(f'{key} must be finite, positive and at most {rule["maximum"]}')
        elif kind == 'array':
            if not isinstance(value, list) or not rule.get('minItems', 0) <= len(value) <= rule.get('maxItems', 256):
                raise ValueError(f'{key} requires a bounded array')
            if key == 'commands':
                from simxr_control.commands import encode_command
                for index, command in enumerate(value):
                    encode_command(command, index + 1)
        elif not isinstance(value, str) or len(value) < rule.get('minLength', 0):
            raise ValueError(f'{key} must be a nonempty string')
        if 'enum' in rule and value not in rule['enum']:
            raise ValueError(f'Unsupported {key}')
    if name in ('debug_step', 'debug_strict_admission') and args.get('target', 'composed_eyes') == 'composed_eyes' and (args.get('layer', 0) or args.get('view', 0)):
        raise ValueError('Composed-eye capture has no layer/view index')
    if name.startswith('game_bridge_') and (('session' in args) != ('generation' in args)):
        raise ValueError('Supply both session and generation, or omit both for the independent game bridge')
    if name == 'simulation_schedule':
        if ('frame' in args) == ('time_ns' in args):
            raise ValueError('Select exactly one frame or time_ns target')
        if 'time_ns' in args:
            value = args['time_ns']
            if not isinstance(value, str) or re.fullmatch(r'0|[1-9][0-9]*', value) is None or int(value) >= 2**63:
                raise ValueError('time_ns requires a canonical nonnegative int64 string')
            normalized['time_ns'] = int(value)
    return normalized


_IDENTIFIER_FIELDS = {'session', 'generation', 'lease', 'command_id', 'operation_id', 'capture_id',
                      'frame_id', 'tick', 'next_admission', 'last_submitted', 'request_id', 'revision_id', 'inspection_id',
                      'game_id', 'transaction_id', 'applied_frame', 'origin_frame', 'seed', 'identity', 'image_reader_id', 'delivery_id', 'export_id', 'pool_generation', 'shared_image_id'}
_IDENTIFIER_LISTS = {'captures', 'diagnostics'}


def _json_result(value, key=None):
    if isinstance(value, bytes):
        if key in ('device_uuid', 'driver_uuid'):
            return value.hex()
        return {'encoding': 'base64', 'data': base64.b64encode(value).decode('ascii')}
    if isinstance(value, dict):
        return {name: _json_result(item, name) for name, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_result(item, 'capture_id' if key in _IDENTIFIER_LISTS else None) for item in value]
    if type(value) is int and (key in _IDENTIFIER_FIELDS or abs(value) > _SAFE_INTEGER):
        return str(value)
    return value


def _safe_close(client):
    try:
        client.close()
    except Exception:
        pass


class DebugTools:
    def __init__(self, *, max_connections=16, client_factory=None):
        if type(max_connections) is not int or not 1 <= max_connections <= 16:
            raise ValueError('Debugger connection capacity must be in [1,16]')
        self._capacity = max_connections
        self._factory = client_factory
        self._lock = threading.Lock()
        self._clients = {}
        self._closed = False

    def _lookup(self, identity):
        with self._lock:
            if self._closed:
                raise ConnectionError('Debugger adapter closed')
            if identity not in self._clients:
                raise LookupError('Unknown debugger connection')
            client = self._clients[identity]
        if client is None:
            raise BlockingIOError('Debugger connection is still opening')
        return client

    def _connect(self, args, sdk):
        with self._lock:
            if self._closed:
                return {'ok': False, 'error': 'closed'}
            if len(self._clients) >= self._capacity:
                return {'ok': False, 'error': 'capacity'}
            identity = 'debug_' + uuid.uuid4().hex
            self._clients[identity] = None
        client = None
        published = False
        try:
            factory = self._factory or sdk.Client
            client = factory(args.get('path'), timeout=args.get('timeout', 2.0), name=args.get('name', 'MCP debugger'))
            with self._lock:
                if not self._closed and identity in self._clients:
                    self._clients[identity] = client
                    published = True
            if not published:
                _safe_close(client)
                return {'ok': False, 'error': 'closed'}
            sessions = client.sessions()
            return {'ok': True, 'connection': identity, 'hello': client.hello,
                    'sessions': [{'session': session.id, 'generation': session.generation} for session in sessions]}
        except BaseException as error:
            if published and isinstance(error, sdk.ControlTimeout):
                # Discovery may be unfinished after Hello succeeded. Keep its
                # exact exchange and return the identity needed to finish it.
                return self._failure(error, sdk, identity)
            with self._lock:
                self._clients.pop(identity, None)
            if client is not None:
                _safe_close(client)
            if isinstance(error, (KeyboardInterrupt, SystemExit)):
                raise
            result = self._failure(error, sdk)
            if isinstance(error, sdk.ControlTimeout):
                result['retry_needed'] = False
                result['reconnect_needed'] = True
            return result

    @staticmethod
    def _failure(error, sdk, connection=None):
        result = {'ok': False}
        if connection is not None:
            result['connection'] = connection
        if isinstance(error, sdk.ControlError):
            result.update(error='control_error', code=int(error.code), record=error.record)
        elif isinstance(error, sdk.ControlTimeout):
            result.update(error='unfinished', unfinished=True, request_id=error.request_id, retry_needed=True)
        elif isinstance(error, sdk.ProtocolError):
            result.update(error='protocol_error', detail=str(error))
        elif isinstance(error, BlockingIOError):
            result.update(error='busy', detail=str(error))
        elif isinstance(error, LookupError):
            result.update(error='not_found', detail=str(error))
        elif isinstance(error, (ValueError, TypeError)):
            result.update(error='invalid_arguments', detail=str(error))
        elif isinstance(error, (ConnectionError, OSError)):
            result.update(error='transport_error', detail=str(error))
        else:
            result.update(error='internal_error', detail=str(error))
        return result

    def call(self, name, args):
        return _json_result(self._call(name, args))

    def _call(self, name, args):
        try:
            args = _validate(name, args)
        except LookupError as error:
            return {'ok': False, 'error': 'unsupported_tool', 'detail': str(error)}
        except (ValueError, TypeError) as error:
            return {'ok': False, 'error': 'invalid_arguments', 'detail': str(error)}
        try:
            sdk = _sdk()
        except Exception as error:
            return {'ok': False, 'error': 'unavailable', 'detail': str(error)}
        if name == 'debug_connect':
            return self._connect(args, sdk)
        identity = args['connection']
        try:
            if name == 'debug_disconnect':
                with self._lock:
                    if identity not in self._clients:
                        raise LookupError('Unknown debugger connection')
                    client = self._clients.pop(identity)
                if client is not None:
                    client.close()
                return {'ok': True, 'connection': identity, 'disconnected': True}
            client = self._lookup(identity)
            if name == 'debug_retry':
                record = client.retry()
            elif name == 'debug_revision':
                record = client.revision(args['revision'])
            elif name == 'debug_release_inspection':
                record = client.release_inspection(args['inspection'])
            elif name in ('debug_image_info', 'debug_release_image'):
                record = getattr(client, name.removeprefix('debug_'))(args['image'])
            elif name in ('debug_delivery_info', 'debug_release_delivery'):
                record = getattr(client, name.removeprefix('debug_'))(args['delivery'])
            elif name == 'debug_start_export':
                record = client.start_export(args['image'], layer=args.get('layer'))
            elif name in ('debug_export_info', 'debug_release_export'):
                record = getattr(client, name.removeprefix('debug_'))(args['export'])
            elif name == 'debug_read_export':
                record = client.read_export(args['export'], offset=args.get('offset', 0),
                                            length=args.get('length', 1024 * 1024 - 4096))
            else:
                session = sdk.Session(args['session'], args['generation']) if 'session' in args else None
                if name == 'game_bridge_status':
                    record = client.game_status(session)
                elif name == 'game_bridge_command':
                    record = client.game_command(session, args['game'], args['kind'], args.get('payload', ''), command_id=args['command_id'])
                elif name == 'game_bridge_operation':
                    record = client.game_operation(session, args['game'], args['operation'])
                elif name == 'simulation_schedule':
                    policy = sdk.LatePolicy.RESCHEDULE_NEXT if args.get('late_policy') == 'reschedule_next' else sdk.LatePolicy.REJECT
                    record = client.schedule(session, args['commands'], frame=args.get('frame'), time_ns=args.get('time_ns'),
                                             transaction_id=args['transaction_id'], late_policy=policy)
                elif name == 'simulation_status':
                    record = client.simulation_status(session, args.get('transaction_id'))
                elif name == 'simulation_cancel':
                    record = client.cancel_simulation(session, args['transaction_id'])
                elif name == 'simulation_release_control':
                    record = client.release_control(session)
                elif name in ('simulation_start_recording', 'simulation_stop_recording'):
                    record = getattr(client, name.removeprefix('simulation_'))(session)
                elif name == 'simulation_diagnostics':
                    record = client.diagnostics(session, after=args.get('after', 0), count=args.get('count', 128), frame=args.get('frame'))
                elif name == 'simulation_read_recording':
                    record = client.read_recording(session, offset=args.get('offset', 0), count=args.get('count', 16))
                elif name == 'debug_status':
                    record = client.status(session)
                elif name == 'debug_acquire':
                    lease = client.acquire(session)
                    return {'ok': True, 'connection': identity, 'session': session.id,
                            'generation': session.generation, 'lease': lease.id}
                elif name == 'debug_operation':
                    record = client.operation(session, args['operation'])
                elif name == 'debug_capture':
                    record = client.capture(session, args['capture'])
                elif name == 'debug_inspect':
                    record = client.inspect(session, args['capture'])
                elif name == 'debug_retain_image':
                    selection = {}
                    if 'target' in args:
                        selection = dict(target=sdk.TargetKind[args['target'].upper()],
                                         layer=args.get('layer', 0), view=args.get('view', 0))
                    record = client.retain_image(session, args['capture'], **selection)
                elif name == 'debug_retain_preview':
                    record = client.retain_preview(session)
                else:
                    lease = sdk.Lease(session, args['lease'])
                    if name == 'debug_renew':
                        record = client.renew(lease)
                    elif name == 'debug_relinquish':
                        record = client.relinquish(lease)
                    elif name == 'debug_acknowledge':
                        record = client.acknowledge(lease, args['through'])
                    else:
                        target = sdk.TargetKind[args.get('target', 'composed_eyes').upper()]
                        extra = {'command_id': args['command_id']}
                        if name in ('debug_step', 'debug_strict_admission'):
                            extra.update(target=target, layer=args.get('layer', 0), view=args.get('view', 0),
                                         lookahead=args.get('lookahead', 1))
                        if name == 'debug_cancel':
                            extra['operation'] = args['operation']
                        if name == 'debug_release':
                            extra['capture'] = args['capture']
                        record = client.submit(lease, getattr(sdk.CommandKind, _COMMAND_NAMES[name]), **extra)
            result = {**record, 'ok': True, 'connection': identity}
            if name in ('debug_capture', 'debug_retry', 'debug_inspect', 'debug_revision'):
                result.update(metadata_only='bytes' not in record, gpu_preview_supported=False)
            if name == 'debug_read_export':
                result['metadata_only'] = False
            return result
        except Exception as error:
            return self._failure(error, sdk, identity)

    def close(self):
        with self._lock:
            self._closed = True
            clients = [client for client in self._clients.values() if client is not None]
            self._clients.clear()
        for client in clients:
            _safe_close(client)
