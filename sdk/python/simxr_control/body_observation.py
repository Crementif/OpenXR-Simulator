"""Runtime-estimated body observations; joint IDs use simulator semantics."""
import math
from .protocol import Field, Tag, ProtocolError, decode_fields


# Membership of BodyModel::JointMap, independent of native extension enum order.
JOINT_SETS = (
    frozenset((
        0, 1, 2, 3, 4, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
        38, 39, 41, 42, 43, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
        60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75,
        77, 78, 80, 81, 82, 83,
    )),
    frozenset((
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
        32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
        48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
        64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
        80, 81, 82, 83,
    )),
    frozenset((
        1, 2, 5, 7, 37, 38, 40, 41, 44, 46, 76, 77, 79, 81, 82, 83,
    )),
    frozenset((
        1, 2, 3, 4, 5, 7, 25, 35, 37, 38, 40, 41, 42, 43, 44, 46,
        64, 74, 76, 77, 79, 81, 82, 83,
    )),
)


def _integer(value, maximum, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ProtocolError('Invalid observed body integer')
    return value


def _pose(entry):
    position, orientation = entry['position'], entry['orientation']
    for values, count in ((position, 3), (orientation, 4)):
        if not isinstance(values, (list, tuple)) or len(values) != count or any(
                type(v) not in (int, float) or not math.isfinite(v) for v in values):
            raise ProtocolError('Observed body requires complete finite poses')
    if abs(math.sqrt(sum(v * v for v in orientation)) - 1) > 0.01:
        raise ProtocolError('Invalid observed body orientation')
    return Field.nested(Tag.POSE, [Field.f32(i + 1, v, True)
                                  for i, v in enumerate(list(orientation) + list(position))], True)


def encode_observation(body):
    if not isinstance(body, dict) or set(body) - {
            'joint_set', 'requested_time', 'returned_time', 'located', 'active',
            'all_joint_poses_tracked', 'confidence', 'skeleton_revision', 'joints', 'skeleton'}:
        raise ProtocolError('Invalid body observation fields')
    joint_set = _integer(body['joint_set'], 3)
    fields = [Field.u32(1, joint_set, True),
              Field.i64(2, _integer(body['requested_time'], 2**63 - 1, 1), True),
              Field.boolean(4, body['located'], True)]
    fb = joint_set < 2
    metadata = set(body) & {'returned_time', 'active', 'confidence', 'skeleton_revision',
                           'skeleton', 'all_joint_poses_tracked'}
    required = ({'returned_time', 'active', 'confidence', 'skeleton_revision', 'skeleton'}
                if fb else {'all_joint_poses_tracked'}) if body['located'] else set()
    if metadata != required:
        raise ProtocolError('Observed body metadata does not match provider/activity')
    if 'returned_time' in body:
        fields.append(Field.i64(3, _integer(body['returned_time'], 2**63 - 1,
                                           1 if body['active'] else 0), True))
    for key, tag in (('active', 5), ('all_joint_poses_tracked', 6)):
        if key in body:
            fields.append(Field.boolean(tag, body[key], True))
    if 'confidence' in body:
        value = body['confidence']
        if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1:
            raise ProtocolError('Invalid body confidence')
        fields.append(Field.f32(7, value, True))
    if 'skeleton_revision' in body:
        fields.append(Field.u32(8, _integer(body['skeleton_revision'], 2**32 - 1), True))
    for key, tag in (('joints', 9), ('skeleton', 10)):
        if key == 'skeleton' and key not in body:
            continue
        entries = body[key]
        if not isinstance(entries, list) or len(entries) > 84:
            raise ProtocolError('Observed joints require a bounded list')
        seen, parents, encoded = set(), {}, []
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) - {
                    'joint', 'position', 'orientation', 'parent' if key == 'skeleton' else 'flags'}:
                raise ProtocolError('Invalid observed joint fields')
            joint = _integer(entry['joint'], 83)
            if joint in seen:
                raise ProtocolError('Duplicate observed joint')
            seen.add(joint)
            values = [_pose(entry)]
            if key == 'joints':
                flags = _integer(entry['flags'], 15)
                if (flags & 4 and not flags & 1 or flags & 8 and not flags & 2 or
                        (not body['located'] or body.get('active') is False) and flags):
                    raise ProtocolError('Invalid observed joint validity')
                values.append(Field.u64(Tag.USAGE, flags, True))
            elif 'parent' in entry:
                parent = _integer(entry['parent'], 83)
                parents[joint] = parent
                values.append(Field.u32(Tag.CHANNEL, parent, True))
            encoded.append(Field.nested(joint + 1, values, True))
        if seen != JOINT_SETS[joint_set]:
            raise ProtocolError('Observed joints do not match provider schema')
        for joint in parents:
            visited = {joint}
            while joint in parents:
                joint = parents[joint]
                if joint not in seen or joint in visited:
                    raise ProtocolError('Invalid observed skeleton hierarchy')
                visited.add(joint)
        fields.append(Field.nested(tag, sorted(encoded, key=lambda entry: entry.tag), True))
    return Field.nested(Tag.BODY_OBSERVATION, fields, True)


def decode_observation(field):
    try:
        fields = decode_fields(field.value)
        if set(fields) - set(range(1, 11)):
            raise ProtocolError('Unknown observed body fields')
        body = dict(joint_set=fields[1].as_u32(), requested_time=fields[2].as_i64(),
                    located=fields[4].as_bool())
        for key, tag, read in (('returned_time', 3, Field.as_i64), ('active', 5, Field.as_bool),
                               ('all_joint_poses_tracked', 6, Field.as_bool),
                               ('confidence', 7, Field.as_f32), ('skeleton_revision', 8, Field.as_u32)):
            if tag in fields:
                body[key] = read(fields[tag])
        for key, tag in (('joints', 9), ('skeleton', 10)):
            if key == 'skeleton' and tag not in fields:
                continue
            entries = decode_fields(fields[tag].value)
            if list(entries) != sorted(entries) or any(not 1 <= i <= 84 for i in entries):
                raise ProtocolError('Invalid observed joint order/range')
            body[key] = []
            for identity, entry in entries.items():
                parts = decode_fields(entry.value)
                allowed = {Tag.POSE, Tag.CHANNEL if key == 'skeleton' else Tag.USAGE}
                if set(parts) - allowed:
                    raise ProtocolError('Unknown observed joint fields')
                pose = decode_fields(parts[Tag.POSE].value)
                if list(pose) != list(range(1, 8)):
                    raise ProtocolError('Invalid observed joint pose')
                values = [v.as_f32() for v in pose.values()]
                joint = dict(joint=identity - 1, orientation=values[:4], position=values[4:])
                if key == 'joints':
                    joint['flags'] = parts[Tag.USAGE].as_u64()
                elif Tag.CHANNEL in parts:
                    joint['parent'] = parts[Tag.CHANNEL].as_u32()
                body[key].append(joint)
        encode_observation(body)
        return body
    except (KeyError, TypeError, ValueError, OverflowError) as error:
        if isinstance(error, ProtocolError):
            raise
        raise ProtocolError('Malformed observed body') from error
