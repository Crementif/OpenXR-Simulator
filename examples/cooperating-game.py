#!/usr/bin/env python3
"""Tiny game model demonstrating the optional simxr/game_bridge.h C ABI.

Without --runtime it advances normally. With a bridge it waits for reset/step
commands, applies them at this loop's safe update point, then acknowledges them.
The model has no XR instance or GPU; real games use their own safe update point.
"""
import argparse
import ctypes as c
import json
import signal
import time


class Command(c.Structure):
    _fields_ = [('id', c.c_uint64), ('kind', c.c_uint32), ('payload_length', c.c_uint32),
                ('payload', c.c_char * 4096)]


class BridgeApi(c.Structure):
    _fields_ = [
        ('version', c.c_uint32), ('size', c.c_uint32), ('context', c.c_void_p),
        ('register_game', c.CFUNCTYPE(c.c_uint64, c.c_void_p, c.c_uint32)),
        ('unregister_game', c.CFUNCTYPE(None, c.c_void_p, c.c_uint64)),
        ('poll', c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_uint64, c.POINTER(Command))),
        ('complete', c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_uint64, c.c_uint64, c.c_int, c.c_char_p, c.c_uint32)),
        ('publish', c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_uint64, c.c_char_p, c.c_uint32)),
        ('shutdown', c.CFUNCTYPE(c.c_int, c.c_void_p)),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', help='Absolute simulator runtime library path; optional')
    parser.add_argument('--duration', type=float, default=60, help='Run time in seconds (default: 60)')
    args = parser.parse_args()
    if not 0 < args.duration <= 3600:
        parser.error('--duration must be in (0, 3600]')
    running = True

    def stop(_signal, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    api = None
    game = 0
    negotiated = False
    ticks = 0
    library = None
    try:
        if args.runtime:
            library = c.CDLL(args.runtime)
            negotiate = library.simxrGetGameBridgeApi
            negotiate.argtypes = [c.c_uint32, c.POINTER(BridgeApi)]
            negotiate.restype = c.c_int
            api = BridgeApi()
            api.size = c.sizeof(api)
            if not negotiate(1, c.byref(api)) or api.version != 1:
                raise RuntimeError('Runtime does not provide game bridge ABI v1')
            negotiated = True
            game = api.register_game(api.context, 1 | 2)  # Actual RESET and STEP support.
            if not game:
                raise RuntimeError('Game registration failed')

        def publish():
            telemetry = json.dumps({'ticks': ticks, 'position': ticks * .1}).encode()
            if api and not api.publish(api.context, game, telemetry, len(telemetry)):
                raise RuntimeError('Game telemetry publication failed')
            return telemetry.decode()

        print('READY ' + publish(), flush=True)
        deadline = time.monotonic() + args.duration
        while running and time.monotonic() < deadline:
            # This is the game-owned safe update point. No game mutation runs on an XR/control thread.
            if api:
                command = Command()
                if api.poll(api.context, game, c.byref(command)):
                    if command.payload_length > 4096:
                        raise RuntimeError('Bridge returned an oversized command')
                    payload = c.string_at(c.addressof(command) + Command.payload.offset,
                                          command.payload_length).decode('utf-8')
                    success = True
                    if command.kind == 1 and payload == 'checkpoint:start':
                        ticks = 0
                        result = 'reset applied'
                    elif command.kind == 2 and payload.isascii() and payload.isdecimal() and 0 < int(payload) <= 1000:
                        ticks += int(payload)
                        result = 'step applied'
                    else:
                        success = False
                        result = 'unsupported reset checkpoint or invalid step count'
                    publish()
                    encoded = result.encode()
                    if not api.complete(api.context, game, command.id, success, encoded, len(encoded)):
                        raise RuntimeError('Game acknowledgement failed')
            else:
                ticks += 1
            time.sleep(.01)
        print('STOP ' + publish(), flush=True)
    finally:
        if negotiated:
            if game:
                api.unregister_game(api.context, game)
            # This sample owns the loaded runtime and creates no XR instances.
            # A game with XR calls destroys those instances before this optional unload boundary.
            if not api.shutdown(api.context):
                raise RuntimeError('Runtime still has live XR owners at shutdown')
        # Keep the library and all ABI function pointers alive through unregister/shutdown.
        del library


if __name__ == '__main__':
    main()
