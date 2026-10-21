"""Typed, editable simulator commands shared by SDK, MCP and recorded rigs."""
from enum import IntEnum
import math
from .protocol import Field, Tag, ProtocolError, decode_fields


class SourceKind(IntEnum):
    MANUAL = 0
    MCP = 1
    RIG = 2


class LatePolicy(IntEnum):
    REJECT = 0
    RESCHEDULE_NEXT = 1


class ControlChannel(IntEnum):
    HEAD = 0
    LEFT_POSE = 1
    RIGHT_POSE = 2
    LEFT_CONTROLS = 3
    RIGHT_CONTROLS = 4
    BODY = 5
    GAZE = 6
    TRACKERS = 7
    VIEWS = 8
    PROPORTIONS = 9
    PROFILES = 10
    SESSION = 11


class Scenario(IntEnum):
    HEAD_UNTRACKED = 0
    HEAD_UNAVAILABLE = 1
    BODY_UNTRACKED = 2
    BODY_UNAVAILABLE = 3
    GAZE_UNTRACKED = 4
    GAZE_UNAVAILABLE = 5
    LEFT_DISCONNECTED = 6
    RIGHT_DISCONNECTED = 7
    TRACKERS_DISCONNECTED = 8
    UNFOCUSED = 9
    HIDDEN = 10
    RECENTER = 11
    SESSION_LOSS = 12


_KINDS = {'take_control': 1, 'release_control': 2, 'head_pose': 3, 'controller_pose': 4,
          'controller_controls': 5, 'body': 6, 'gaze': 7, 'tracker': 8, 'views': 9,
          'proportions': 10, 'controller_profile': 11, 'scenario': 12, 'reference_spaces': 13}
_CONTROL_FLOATS = ('trigger', 'grip', 'grip_force', 'stick_x', 'stick_y', 'trackpad_x', 'trackpad_y', 'trackpad_force')
_CONTROL_BOOLS = ('primary', 'secondary', 'menu', 'back', 'thumb_click', 'trackpad_click',
                  'primary_touch', 'secondary_touch', 'trigger_touch', 'thumb_touch', 'trackpad_touch', 'thumbrest_touch', 'connected')


def _enum(value, cls):
    try:
        return cls[value.upper()] if isinstance(value, str) else cls(value)
    except (KeyError, ValueError, TypeError) as error:
        raise ProtocolError(f'Invalid {cls.__name__}: {value}') from error


def _floats(tag, values, count):
    if not isinstance(values, (list, tuple)) or len(values) != count or any(
            type(v) not in (float, int) or not math.isfinite(v) for v in values):
        raise ProtocolError(f'Expected {count} finite numbers')
    return Field.nested(tag, [Field.f32(i + 1, v, True) for i, v in enumerate(values)], True)


def _uint(value, maximum=2**32 - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ProtocolError('Expected an unsigned integer')
    return value


def _pose(command):
    return _floats(Tag.POSE, list(command.get('orientation', [0, 0, 0, 1])) + list(
        command.get('position', [0, 0, 0])), 7)


def _encode_command(command, index=1):
    """Encode a command dictionary. Unspecified fields use documented neutral defaults.

    Ownership is explicit: place take_control before setters in the same transaction.
    replace=True is an explicit request to take another source's channel.
    """
    if not isinstance(command, dict) or command.get('kind') not in _KINDS:
        raise ProtocolError('A known command kind is required')
    kind = command['kind']
    allowed = {'kind'}
    f = []
    def allow(*names):
        allowed.update(names)
    if kind in ('take_control', 'release_control'):
        allow('channel', 'replace')
        f = [Field.u32(Tag.CHANNEL, _enum(command['channel'], ControlChannel), True)]
        if kind == 'take_control':
            f.append(Field.boolean(Tag.TAKE_CONTROL, command.get('replace', False), True))
        elif command.get('replace', False):
            raise ProtocolError('release_control has no replace option')
    elif kind in ('head_pose', 'controller_pose'):
        allow('position', 'orientation', 'flags')
        f = [_pose(command), Field.u64(Tag.USAGE, _uint(command.get('flags', 15), 15), True)]
        if kind == 'controller_pose':
            allow('hand', 'connected')
            f += [Field.u32(Tag.LAYER, _uint(command.get('hand', 0), 1), True),
                  Field.boolean(Tag.ENABLED, command.get('connected', True), True)]
    elif kind == 'controller_controls':
        allow('hand', *_CONTROL_FLOATS, *_CONTROL_BOOLS)
        values = [Field.f32(i + 1, command.get(name, 0.0), True) for i, name in enumerate(_CONTROL_FLOATS)]
        values += [Field.boolean(i + 9, command.get(name, name == 'connected'), True)
                   for i, name in enumerate(_CONTROL_BOOLS)]
        f = [Field.u32(Tag.LAYER, _uint(command.get('hand', 0), 1), True), Field.nested(Tag.CONTROLS, values, True)]
    elif kind == 'body':
        allow('active', 'confidence', 'preset', 'controls', 'joints')
        controls = command.get('controls', [{} for _ in range(5)])
        if not isinstance(controls, list) or len(controls) != 5:
            raise ProtocolError('Body controls requires five entries')
        encoded_controls = []
        for i, control in enumerate(controls):
            if not isinstance(control, dict) or set(control) - {'tracked', 'offset'}:
                raise ProtocolError('Invalid body control')
            fields = [Field.boolean(Tag.ENABLED, control.get('tracked', True), True)]
            if control.get('offset') is not None:
                fields.append(_floats(Tag.POSITION, control['offset'], 3))
            encoded_controls.append(Field.nested(i + 1, fields, True))
        joints = []
        entries = command.get('joints', [])
        if not isinstance(entries, list) or len(entries) > 84:
            raise ProtocolError('Body joints must be a bounded list')
        for joint in entries:
            if not isinstance(joint, dict) or set(joint) - {'joint', 'tracked', 'position', 'orientation'}:
                raise ProtocolError('Invalid body joint')
            fields = [Field.boolean(Tag.ENABLED, joint.get('tracked', True), True)]
            if 'position' in joint or 'orientation' in joint:
                fields.append(_pose(joint))
            joints.append(Field.nested(_uint(joint['joint'], 83) + 1, fields, True))
        f = [Field.boolean(Tag.ENABLED, command.get('active', True), True),
             Field.f32(Tag.VALUES, command.get('confidence', 1.0), True),
             Field.u32(Tag.KIND, _uint(command.get('preset', 0), 2), True),
             Field.nested(Tag.CONTROLS, encoded_controls, True), Field.nested(Tag.PROPORTIONS, joints, True)]
    elif kind == 'gaze':
        allow('yaw', 'pitch', 'available', 'tracked')
        f = [_floats(Tag.VALUES, [command.get('yaw', 0), command.get('pitch', 0)], 2),
             Field.boolean(Tag.ENABLED, command.get('available', True), True),
             Field.boolean(Tag.RUNNING, command.get('tracked', True), True)]
    elif kind == 'tracker':
        allow('index', 'identity', 'role', 'connected', 'tracked', 'anchor', 'position', 'orientation')
        f = [Field.u32(Tag.LAYER, _uint(command.get('index', 0), 3), True),
             Field.u64(Tag.SOURCE, _uint(command['identity'], 2**64 - 1), True),
             Field.u32(Tag.KIND, _uint(command.get('role', 4), 17), True),
             Field.boolean(Tag.ENABLED, command.get('connected', True), True),
             Field.boolean(Tag.RUNNING, command.get('tracked', True), True)]
        if command.get('anchor') is not None:
            f.append(Field.u32(Tag.CHANNEL, _uint(command['anchor'], 83), True))
        if 'position' in command or 'orientation' in command:
            f.append(_pose(command))
    elif kind == 'views':
        allow('ipd', 'inset_half_angle', 'outer_fov', 'outer_width', 'outer_height', 'inset_width', 'inset_height')
        fov = command.get('outer_fov', [-0.9, 0.9, 0.9, -0.9] * 2)
        f = [_floats(Tag.VALUES, [command.get('ipd', 0.064), command.get('inset_half_angle', 0.35)] + list(fov), 10),
             Field.u32(Tag.WIDTH, _uint(command.get('outer_width', 1440)), True),
             Field.u32(Tag.HEIGHT, _uint(command.get('outer_height', 1600)), True),
             Field.u32(Tag.DEPTH, _uint(command.get('inset_width', 720)), True),
             Field.u32(Tag.ARRAY_LAYERS, _uint(command.get('inset_height', 800)), True)]
    elif kind == 'proportions':
        allow('height', 'shoulder_width', 'hip_width')
        f = [_floats(Tag.VALUES, [command.get('height', 1.7), command.get('shoulder_width', 0.38), command.get('hip_width', 0.24)], 3)]
    elif kind == 'controller_profile':
        allow('hand', 'path')
        f = [Field.u32(Tag.LAYER, _uint(command.get('hand', 0), 1), True), Field.text(Tag.DETAIL, command['path'], True)]
    elif kind == 'scenario':
        allow('scenario', 'enabled')
        f = [Field.u32(Tag.SCENARIO, _enum(command['scenario'], Scenario), True),
             Field.boolean(Tag.ENABLED, command.get('enabled', True), True)]
    elif kind == 'reference_spaces':
        allow('local_position', 'local_orientation', 'stage_position', 'stage_orientation',
              'bounds_available', 'stage_bounds')
        available = command.get('bounds_available', True)
        bounds = command.get('stage_bounds', [2, 2] if available else [0, 0])
        if not available and bounds != [0, 0]:
            raise ProtocolError('Unavailable stage bounds must have zero extent')
        f = [_floats(1, list(command.get('local_orientation', [0, 0, 0, 1])) +
                     list(command.get('local_position', [0, 0, 0])), 7),
             _floats(2, list(command.get('stage_orientation', [0, 0, 0, 1])) +
                     list(command.get('stage_position', [0, 0, 0])), 7),
             Field.boolean(3, available, True), _floats(4, bounds, 2)]
    if set(command) - allowed:
        raise ProtocolError('Unknown command arguments: ' + ', '.join(sorted(set(command) - allowed)))
    return Field.nested(index, [Field.u32(Tag.KIND, _KINDS[kind], True), Field.nested(Tag.VALUES, f, True)], True)


def decode_command(record):
    """Decode the normalized canonical command retained in a recording."""
    if not isinstance(record, dict) or record.get('encoding') != 'simxr-command-v1-hex':
        raise ProtocolError('Unsupported recorded command encoding')
    fields = decode_fields(bytes.fromhex(record['data']))
    kind = next((name for name, value in _KINDS.items() if value == fields[Tag.KIND].as_u32()), None)
    if kind is None:
        raise ProtocolError('Unsupported recorded command')
    f = decode_fields(fields[Tag.VALUES].value)
    c = {'kind': kind}
    def floats(tag):
        return [v.as_f32() for v in decode_fields(f[tag].value).values()]
    def pose():
        values = floats(Tag.POSE)
        c.update(orientation=values[:4], position=values[4:])
    if kind in ('take_control', 'release_control'):
        c['channel'] = ControlChannel(f[Tag.CHANNEL].as_u32()).name.lower()
        if kind == 'take_control': c['replace'] = f[Tag.TAKE_CONTROL].as_bool()
    elif kind in ('head_pose', 'controller_pose'):
        pose(); c['flags'] = f[Tag.USAGE].as_u64()
        if kind == 'controller_pose': c.update(hand=f[Tag.LAYER].as_u32(), connected=f[Tag.ENABLED].as_bool())
    elif kind == 'controller_controls':
        controls = decode_fields(f[Tag.CONTROLS].value)
        c['hand'] = f[Tag.LAYER].as_u32()
        c.update({name: controls[i + 1].as_f32() for i, name in enumerate(_CONTROL_FLOATS)})
        c.update({name: controls[i + 9].as_bool() for i, name in enumerate(_CONTROL_BOOLS)})
    elif kind == 'body':
        c.update(active=f[Tag.ENABLED].as_bool(), confidence=f[Tag.VALUES].as_f32(), preset=f[Tag.KIND].as_u32(), controls=[], joints=[])
        for field in decode_fields(f[Tag.CONTROLS].value).values():
            control = decode_fields(field.value)
            value = {'tracked': control[Tag.ENABLED].as_bool()}
            if Tag.POSITION in control:
                value['offset'] = [v.as_f32() for v in decode_fields(control[Tag.POSITION].value).values()]
            c['controls'].append(value)
        for index, field in decode_fields(f[Tag.PROPORTIONS].value).items():
            joint = decode_fields(field.value)
            value = {'joint': index - 1, 'tracked': joint[Tag.ENABLED].as_bool()}
            if Tag.POSE in joint:
                values = [v.as_f32() for v in decode_fields(joint[Tag.POSE].value).values()]
                value.update(orientation=values[:4], position=values[4:])
            c['joints'].append(value)
    elif kind == 'gaze':
        values = floats(Tag.VALUES)
        c.update(yaw=values[0], pitch=values[1], available=f[Tag.ENABLED].as_bool(), tracked=f[Tag.RUNNING].as_bool())
    elif kind == 'tracker':
        c.update(index=f[Tag.LAYER].as_u32(), identity=f[Tag.SOURCE].as_u64(), role=f[Tag.KIND].as_u32(), connected=f[Tag.ENABLED].as_bool(), tracked=f[Tag.RUNNING].as_bool())
        if Tag.CHANNEL in f: c['anchor'] = f[Tag.CHANNEL].as_u32()
        if Tag.POSE in f: pose()
    elif kind == 'views':
        v = floats(Tag.VALUES)
        c.update(ipd=v[0], inset_half_angle=v[1], outer_fov=v[2:], outer_width=f[Tag.WIDTH].as_u32(), outer_height=f[Tag.HEIGHT].as_u32(), inset_width=f[Tag.DEPTH].as_u32(), inset_height=f[Tag.ARRAY_LAYERS].as_u32())
    elif kind == 'proportions':
        v = floats(Tag.VALUES); c.update(height=v[0], shoulder_width=v[1], hip_width=v[2])
    elif kind == 'controller_profile': c.update(hand=f[Tag.LAYER].as_u32(), path=f[Tag.DETAIL].as_text())
    elif kind == 'scenario': c.update(scenario=Scenario(f[Tag.SCENARIO].as_u32()).name.lower(), enabled=f[Tag.ENABLED].as_bool())
    elif kind == 'reference_spaces':
        local, stage = floats(1), floats(2)
        c.update(local_orientation=local[:4], local_position=local[4:],
                 stage_orientation=stage[:4], stage_position=stage[4:],
                 bounds_available=f[3].as_bool(), stage_bounds=floats(4))
    return c


def encode_command(command, index=1):
    try:
        return _encode_command(command, index)
    except (KeyError, TypeError, ValueError, OverflowError) as error:
        if isinstance(error, ProtocolError):
            raise
        raise ProtocolError('Malformed simulation command: ' + str(error)) from error
