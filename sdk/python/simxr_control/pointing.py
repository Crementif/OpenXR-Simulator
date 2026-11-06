"""Aim a controller at a point of an eye image, matching the MCP point_controller tool."""
import math

DEFAULT_DISTANCE = 3.0

_TOUCH = ((0.5, 0, 0, 0.8660254), (0, -0.0299991, 0.0950003))
_GRIP_IN_AIM = {profile: _TOUCH for profile in (
    '/interaction_profiles/oculus/touch_controller', '/interaction_profiles/meta/touch_pro_controller',
    '/interaction_profiles/meta/touch_plus_controller', '/interaction_profiles/meta/touch_controller_rift_cv1',
    '/interaction_profiles/meta/touch_controller_quest_1_rift_s', '/interaction_profiles/meta/touch_controller_quest_2')}


def _grip_in_aim(profile, left):
    if profile == '/interaction_profiles/valve/index_controller':
        side = 1 if left else -1
        return ((0.4647712, 0.0244080 * side, 0.0080248 * side, 0.8850580),
                (0.0036100 * side, -0.0707737, 0.0843448))
    return _GRIP_IN_AIM.get(profile)


def _multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _rotate(q, v):
    x, y, z, _ = _multiply(_multiply(q, (*v, 0)), (-q[0], -q[1], -q[2], q[3]))
    return (x, y, z)


def _compose(parent, child):
    offset = _rotate(parent[0], child[1])
    return (_multiply(parent[0], child[0]), tuple(p + o for p, o in zip(parent[1], offset)))


def _inverse(pose):
    q = (-pose[0][0], -pose[0][1], -pose[0][2], pose[0][3])
    return (q, tuple(-v for v in _rotate(q, pose[1])))


def _pose(value):
    return (tuple(value['orientation']), tuple(value['position']))


def _command_pose(pose):
    return {'position': list(pose[1]), 'orientation': list(pose[0]), 'flags': 15}


def point_controller(configuration, hand, x, y, *, eye=0, distance=DEFAULT_DISTANCE):
    """A controller_pose command turning `hand` so its aim ray passes through (x, y) of an eye image.

    configuration is simulation_status()['document']['configuration']; x and y run 0..1 from
    the image's top left. The ray meets the eye's ray `distance` metres out, so set it near the
    target's depth. Schedule it after take_control of the hand's pose channel.
    """
    if hand not in (0, 1) or eye not in (0, 1):
        raise ValueError('hand and eye must be 0 or 1')
    if not (0 <= x <= 1 and 0 <= y <= 1) or not 0 < distance <= 1000:
        raise ValueError('x and y must lie in 0..1 and distance in (0, 1000]')
    eyes = configuration.get('eye_views')
    if eyes:
        view = _pose(eyes['eyes'][eye]['pose'])
        fov = eyes['eyes'][eye]['fov']
    else:
        views = configuration['views']
        view = _compose(_pose(configuration['head']), ((0, 0, 0, 1), ((eye - .5) * views['ipd'], 0, 0)))
        fov = views['outer_fov'][eye * 4:eye * 4 + 4]
    left, right, up, down = (math.tan(angle) for angle in fov)
    direction = (left + x * (right - left), up - y * (up - down), -1)
    scale = distance / math.sqrt(sum(v * v for v in direction))
    target = _compose(view, ((0, 0, 0, 1), tuple(v * scale for v in direction)))[1]

    controller = configuration['controllers'][hand]
    offset = _grip_in_aim(controller.get('profile', ''), hand == 0)
    if controller.get('aim'):
        origin = tuple(controller['aim']['position'])
    else:
        grip = _pose(controller['pose'])
        origin = (_compose(grip, _inverse(offset)) if offset else grip)[1]
    toward = [t - o for t, o in zip(target, origin)]
    yaw = math.atan2(-toward[0], -toward[2])
    pitch = math.atan2(toward[1], math.hypot(toward[0], toward[2]))
    orientation = _multiply((0, math.sin(yaw / 2), 0, math.cos(yaw / 2)), (math.sin(pitch / 2), 0, 0, math.cos(pitch / 2)))
    aim = (orientation, origin)
    return {'kind': 'controller_pose', 'hand': hand, 'connected': controller.get('connected', True),
            **_command_pose(_compose(aim, offset) if offset else aim), 'aim': _command_pose(aim)}
