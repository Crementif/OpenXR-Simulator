"""Validated legacy tool edits translated into atomic simulator transactions.

Values are immutable and independent of a connection. Translation consumes one
observed configuration; it never queries mutable runtime state halfway through
an edit or admits work itself.
"""
from dataclasses import dataclass
import math


TOOLS = frozenset({'set_head_pose', 'set_controller_pose', 'hold_controller_inputs',
                   'set_ipd', 'set_fov'})
_FLOAT_CONTROLS = ('trigger', 'grip', 'stickX', 'stickY')
_BOOLEAN_CONTROLS = ('primary', 'secondary', 'menu', 'thumbClick')
_CONTROL_NAMES = {'thumbClick': 'thumb_click', 'stickX': 'stick_x', 'stickY': 'stick_y'}


def _number(arguments, name, default):
    value = arguments.get(name, default)
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError(f'{name} requires a finite number')
    return float(value)


def _clear(arguments):
    value = arguments.get('clear', False)
    if type(value) is not bool:
        raise ValueError('clear requires a boolean')
    return value


def _multiply(first, second):
    x, y, z, w = first
    u, v, s, t = second
    return (w*u + x*t + y*s - z*v, w*v - x*s + y*t + z*u,
            w*s + x*v - y*u + z*t, w*t - x*u - y*v - z*s)


def _orientation(yaw, pitch, roll):
    yaw_rotation = (0, math.sin(yaw/2), 0, math.cos(yaw/2))
    pitch_rotation = (math.sin(pitch/2), 0, 0, math.cos(pitch/2))
    roll_rotation = (0, 0, math.sin(roll/2), math.cos(roll/2))
    return _multiply(_multiply(yaw_rotation, pitch_rotation), roll_rotation)


def _take(channel):
    return {'kind': 'take_control', 'channel': channel}


@dataclass(frozen=True)
class HeadPoseEdit:
    position: tuple
    orientation: tuple

    def commands(self, configuration=None):
        return [_take('head'), {'kind': 'head_pose', 'position': self.position,
                                'orientation': self.orientation}]


@dataclass(frozen=True)
class ControllerEdit:
    hands: tuple
    local_position: tuple | None
    local_orientation: tuple | None
    edit_controls: bool
    clear_controls: bool
    controls: tuple

    def commands(self, configuration):
        commands = []
        for hand in self.hands:
            side = 'left' if hand == 0 else 'right'
            if self.local_position is not None:
                head = configuration['head']
                orientation = head['orientation']
                conjugate = (-orientation[0], -orientation[1], -orientation[2], orientation[3])
                rotated = _multiply(_multiply(orientation, (*self.local_position, 0)), conjugate)
                position = tuple(a + b for a, b in zip(head['position'], rotated[:3]))
                commands.extend([_take(side + '_pose'), {'kind': 'controller_pose', 'hand': hand,
                    'position': position, 'orientation': _multiply(orientation, self.local_orientation)}])
            if self.edit_controls:
                controls = {'kind': 'controller_controls', 'hand': hand}
                if not self.clear_controls:
                    controls.update(configuration['controllers'][hand]['controls'])
                    controls['stick_x'], controls['stick_y'] = controls.pop('stick')
                    controls['trackpad_x'], controls['trackpad_y'] = controls.pop('trackpad')
                controls.update(self.controls)
                commands.extend([_take(side + '_controls'), controls])
        return commands


@dataclass(frozen=True)
class ViewEdit:
    ipd: float | None = None
    outer_fov: tuple | None = None

    def commands(self, configuration):
        views = dict(configuration['views'], kind='views')
        if self.ipd is not None:
            views['ipd'] = self.ipd
        if self.outer_fov is not None:
            views['outer_fov'] = self.outer_fov
        return [_take('views'), views]


def parse_edit(name, arguments):
    """Validate caller values before observing or mutating any runtime state."""
    if not isinstance(arguments, dict):
        raise ValueError('Tool arguments require an object')
    if name == 'set_head_pose':
        return HeadPoseEdit(tuple(_number(arguments, axis, default)
                                  for axis, default in (('x', 0), ('y', 1.7), ('z', 0))),
                            _orientation(*(math.radians(_number(arguments, axis + '_deg', 0))
                                           for axis in ('yaw', 'pitch', 'roll'))))
    if name in ('set_controller_pose', 'hold_controller_inputs'):
        pose = name == 'set_controller_pose'
        hand = arguments.get('hand', 0 if pose else -1)
        if type(hand) not in (int, float) or hand not in (-1, 0, 1) or (pose and hand == -1):
            raise ValueError('hand requires 0 or 1 (hold_controller_inputs also accepts -1 for both)')
        hands = (0, 1) if hand == -1 else (int(hand),)
        controls = []
        for key in _FLOAT_CONTROLS + _BOOLEAN_CONTROLS:
            if key not in arguments:
                continue
            value = _number(arguments, key, 0)
            if key in _BOOLEAN_CONTROLS:
                if value not in (0, 1):
                    raise ValueError(f'{key} requires 0 or 1')
                value = bool(value)
            else:
                minimum = -1 if key in ('stickX', 'stickY') else 0
                if not minimum <= value <= 1:
                    raise ValueError(f'{key} is outside its input range')
            controls.append((_CONTROL_NAMES.get(key, key), value))
        position = None
        orientation = None
        if pose:
            position = tuple(_number(arguments, axis, default) for axis, default in (
                ('posX', -.2 if hand == 0 else .2), ('posY', -.3), ('posZ', -.4)))
            orientation = _orientation(*(_number(arguments, axis, 0) for axis in ('yaw', 'pitch', 'roll')))
        return ControllerEdit(hands, position, orientation, not pose or 'trigger' in arguments,
                              _clear(arguments), tuple(controls))
    if name == 'set_ipd':
        value = 64 if _clear(arguments) else _number(arguments, 'ipd_mm', 64)
        if not 0 <= value <= 200:
            raise ValueError('ipd_mm requires a number in [0, 200]')
        return ViewEdit(ipd=value / 1000)
    if name == 'set_fov':
        clear = _clear(arguments)
        fov = []
        for side in ('left_eye', 'right_eye'):
            eye = {} if clear else arguments.get(side)
            if not isinstance(eye, dict):
                raise ValueError('set_fov requires both eye objects or clear=true')
            fov.extend(math.radians(_number(eye, key + '_deg', default))
                       for key, default in (('left', -45), ('right', 45), ('up', 45), ('down', -45)))
        return ViewEdit(outer_fov=tuple(fov))
    raise ValueError(f'Unknown compatibility edit: {name}')
