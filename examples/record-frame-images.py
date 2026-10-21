#!/usr/bin/env python3
"""Record consecutive GPU images with explicit controlled pacing and bounded storage."""
import argparse
import json
import signal
import threading

from simxr_control import Client, TargetKind, record_frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket')
    parser.add_argument('directory')
    parser.add_argument('--frames', type=int, required=True)
    parser.add_argument('--mode', choices=['controlled', 'realtime'], required=True)
    parser.add_argument('--buffers', type=int, default=2)
    parser.add_argument('--target', choices=['composed_eyes', 'layer_view', 'layer_depth', 'layer_depth_linear'],
                        default='composed_eyes')
    parser.add_argument('--layer', type=int, default=0)
    parser.add_argument('--view', type=int, default=0)
    args = parser.parse_args()
    cancelled = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: cancelled.set())
    with Client(args.socket, name='record-frame-images') as client:
        session, = client.sessions()
        try:
            summary = record_frames(client, session, args.directory, args.frames, mode=args.mode,
                                    buffers=args.buffers, target=TargetKind[args.target.upper()],
                                    layer=args.layer, view=args.view, cancel=cancelled)
        except InterruptedError:
            print('Cancelled; completed files and recording.json describe the partial recording')
            return 130
    print(json.dumps(summary))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
