#!/usr/bin/env python3
"""Record a short head/left-button sequence as an editable replay rig.

Run only against an application you intend to control:
  PYTHONPATH=sdk/python python3 examples/record-input-sequence.py SOCKET OUTPUT.luau
Then use simxr_runner --socket SOCKET --rig OUTPUT.luau --seed PRINTED_SEED.
Replay restores recorded simulator configuration, not application/game state.
"""
import argparse
import json
from pathlib import Path
import time

from simxr_control import Client, collect_recording, to_luau


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists() or args.output.with_suffix('.json').exists():
        parser.error('Choose a new output path; existing recordings are preserved')
    with Client(args.socket, name='record-input-sequence') as client:
        session, = client.sessions()
        before = client.simulation_status(session)['document']
        if any(owner is not None for owner in before['owners']):
            raise RuntimeError('Release existing input owners before recording this sequence')
        head = before['configuration']['head']
        original = head['position']
        outcomes = []
        client.start_recording(session)
        try:
            for offset, down in [(0.12, True), (-0.12, True), (0, False)]:
                position = [original[0] + offset, *original[1:]]
                queued = client.schedule(session, [
                    {'kind': 'take_control', 'channel': 'head'},
                    {'kind': 'take_control', 'channel': 'left_controls'},
                    {'kind': 'head_pose', 'position': position, 'orientation': head['orientation']},
                    {'kind': 'controller_controls', 'hand': 0, 'primary': down, 'trigger': .5 if down else 0},
                ], time_ns=0, late_policy=1)
                result = client.wait_simulation(session, queued['transaction_id'])
                if result['phase'] != 'applied':
                    raise RuntimeError(f'Recording command did not apply: {result}')
                state = client.simulation_status(session)['document']['configuration']
                if (abs(state['head']['position'][0] - position[0]) > .0001 or
                        state['controllers'][0]['controls']['primary'] != down):
                    raise RuntimeError('Applied configuration differs from the recorded command')
                outcomes.append(result)
                time.sleep(.3)
        finally:
            client.stop_recording(session)
            client.release_control(session)
        recording = collect_recording(client, session)
        script = to_luau(recording)
        with args.output.open('x') as stream:
            stream.write(script)
        with args.output.with_suffix('.json').open('x') as stream:
            json.dump({'recording': recording, 'applied': outcomes}, stream, indent=2)
            stream.write('\n')
        print(f'Saved {args.output}; seed={recording["seed"]}; transactions={recording["total"]}')


if __name__ == '__main__':
    main()
