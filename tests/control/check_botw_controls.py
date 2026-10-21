"""Opt-in T03 check against an isolated, running BetterVR/BotW session.

Usage: python3 tests/control/check_botw_controls.py SOCKET OUTPUT_DIRECTORY
Moves both hands and applies buttons/axes briefly, then releases all ownership.
Requires the game's controller action set to be active (load a save first).
"""
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'sdk/python'))
from simxr_control import Client, CommandKind


def snapshot(client, session):
    lease = client.acquire(session)
    capture = None
    try:
        client.wait(session, client.submit(lease, CommandKind.PAUSE)['operation_id'])
        minimum_frame = client.simulation_status(session)['document']['frame_id']
        # Step first drains admitted frames; those retain their original input.
        for _ in range(16):
            result = client.wait(session, client.submit(lease, CommandKind.STEP)['operation_id'])
            if result['progress'] != 'snapshot_ready':
                raise RuntimeError(f'Game did not submit a capture: {result}')
            capture = result['capture_id']
            if result['frame']['frame_id'] > minimum_frame:
                break
            client.wait(session, client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id'])
            capture = None
        if capture is None:
            raise RuntimeError('No new frame after draining admitted work')
        inspected = client.inspect(session, capture)
        document = inspected['document']
        try:
            dictionary = client.revision(document['actions']['dictionary']['revision_id'])
        finally:
            client.release_inspection(inspected['inspection_id'])
        return dict(frame=document, dictionary=dictionary)
    finally:
        if capture is not None:
            client.wait(session, client.submit(lease, CommandKind.RELEASE, capture=capture)['operation_id'])
        client.wait(session, client.submit(lease, CommandKind.RESUME)['operation_id'])


def close(a, b):
    return abs(a - b) < 0.0001


def main():
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    with Client(sys.argv[1]) as client:
        session, = client.sessions()
        before = client.simulation_status(session)['document']
        if any(owner is not None for owner in before['owners']):
            raise RuntimeError('Release existing input owners before running this isolated-game check')
        poses = [[-.35, 1.55, -.5], [.4, 1.25, -.6]]
        triggers = [.25, .75]
        sticks = [[.2, -.3], [-.4, .5]]
        commands = []
        for hand, name in enumerate(('left', 'right')):
            commands += [dict(kind='take_control', channel=f'{name}_pose'),
                         dict(kind='take_control', channel=f'{name}_controls'),
                         dict(kind='controller_pose', hand=hand, position=poses[hand]),
                         dict(kind='controller_controls', hand=hand, trigger=triggers[hand],
                              stick_x=sticks[hand][0], stick_y=sticks[hand][1], primary=True)]
        try:
            queued = client.schedule(session, commands, time_ns=0, late_policy=1)
            applied = client.wait_simulation(session, queued['transaction_id'])
            assert applied['phase'] == 'applied', applied
            # Observe a subsequent publication/action sync, not just command admission.
            time.sleep(.1)
            evidence = snapshot(client, session)
            (output / 'controls-applied.json').write_text(json.dumps(evidence, indent=2) + '\n')
            frame = evidence['frame']
            assert frame['actions']['available'] and frame['actions']['focused']
            assert frame['actions']['input_revision'] == frame['input']['revision']
            matched = []
            for hand, name in enumerate(('left', 'right')):
                position = frame['tracking']['controllers'][hand]['sample']['pose']['position']
                assert all(close(position[axis], value) for axis, value in zip(('x', 'y', 'z'), poses[hand]))
                path = f'/user/hand/{name}'
                for component, field, expected in (
                    ('/input/trigger/value', 'scalar', triggers[hand]),
                    ('/input/thumbstick', 'vector', sticks[hand]),
                    (f'/input/{"x" if hand == 0 else "a"}/click', 'boolean', True),
                    ('/input/grip/pose', 'active', True),
                ):
                    found = []
                    for action in frame['actions']['actions']:
                        if not any(source['binding']['source'] == path + component for source in action['sources']):
                            continue
                        for value in action['values']:
                            if value['subaction_path'] != path or not value['active']:
                                continue
                            actual = value[field]
                            correct = (all(close(actual[axis], number) for axis, number in zip(('x', 'y'), expected))
                                       if field == 'vector' else close(actual, expected))
                            if correct:
                                found.append(action['id'])
                    assert found, f'No synchronized active action for {path + component}: {expected}'
                    matched.append(dict(source=path + component, actions=found, value=expected))
            (output / 'matched-actions.json').write_text(json.dumps(matched, indent=2) + '\n')
        finally:
            client.release_control(session)
        time.sleep(.1)
        released = snapshot(client, session)
        (output / 'controls-released.json').write_text(json.dumps(released, indent=2) + '\n')
        state = client.simulation_status(session)['document']
        assert all(owner is None for owner in state['owners'])
        for controls in released['frame']['input']['controllers']:
            assert controls['trigger'] == 0 and not controls['primary']
            assert controls['stick'] == dict(x=0, y=0)
        for action in released['frame']['actions']['actions']:
            if not any(action['id'] in item['actions'] for item in matched if not item['source'].endswith('/pose')):
                continue
            for value in action['values']:
                if value['active']:
                    assert not value['boolean'] and value['scalar'] == 0
                    assert value['vector'] == dict(x=0, y=0)
        print('PASS both hand poses, bound button/trigger/stick actions at synchronization, and release without held inputs')


if __name__ == '__main__':
    main()
