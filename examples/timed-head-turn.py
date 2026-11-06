#!/usr/bin/env python3
"""Turn the head at a fixed angular speed, independent of the application's frame rate.

Run only against an application you intend to control:
  PYTHONPATH=sdk/python python examples/timed-head-turn.py --rate 12 --seconds 30
The simulator evaluates the time-keyed rows on every frame, so the script only
starts the sequence and reads its status; it never paces poses itself.
"""
import argparse
import math
import time

from simxr_control import Client


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket', nargs='?')
    parser.add_argument('--rate', type=float, default=12.0, help='degrees per second, counter-clockwise')
    parser.add_argument('--seconds', type=float, default=30.0)
    parser.add_argument('--height', type=float, default=1.7)
    args = parser.parse_args()
    with Client(args.socket, name='timed-head-turn') as client:
        session, = client.sessions()
        clock = client.simulation_clock(session)
        if clock:
            print(f"simulation time {clock['time']} ns, epoch {clock['epoch']} ns")
        # Yaw is in radians and not wrapped, so one segment can turn more than a full circle.
        status = client.run_sequence(session, [
            {'time': 0, 'head': {'x': 0, 'y': args.height, 'z': 0, 'yaw': 0, 'pitch': 0, 'roll': 0}},
            {'time': args.seconds, 'head': {'yaw': math.radians(args.rate * args.seconds)}},
        ])
        sequence = status['id']
        try:
            while client.sequence_status(session, sequence)['document']['status']['active']:
                time.sleep(0.5)
        finally:
            state = client.simulation_status(session)['document']
            print('pending', state['pending'], 'rejected', state['rejected'])
            client.release_control(session)


if __name__ == '__main__':
    main()
