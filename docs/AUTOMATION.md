# Automation and debugging

There are three ways to use the simulator:

- **Manual testing**: play the application in the companion. What you play can be saved as a take and exported as a Luau rig, which is how most rigs start.
- **Automation**: repeatable scenarios that end in a pass or fail, such as CI tests.
- **Automated debugging**: you or an AI agent explore a running application to find a bug.

| Interface | For | Catch |
| --- | --- | --- |
| Companion | Manual testing, recording takes | |
| Luau rigs | Automation | Sandboxed: no libraries or network |
| Python SDK | What a rig can't do, like full-resolution image analysis | You handle connections and cleanup |
| MCP | Debugging with an AI agent | Not repeatable by itself |

Record by hand, automate with Luau, and use Python only when a rig can't do the job. When an agent finds a bug, turn the reproduction into a rig.

## Workflows

### Recording a rig by hand

Play the scenario in the companion (**Pilot keys** in the HUD lists the bindings). Select a range in the timeline and choose **Save as take**. In the Library, **Export editable rig** turns the take into a Luau script; add `simxr.checkpoint` calls to make it a test.

A take replays the simulator's input, not the game's state. Start from the same point, or reset the game through the game bridge.

### Running rigs in CI

```sh
export XR_RUNTIME_JSON=$PWD/out/build/release/bin/openxr_simulator.json
export SIMXR_DETERMINISTIC=1
./my_xr_app &
simxr_runner --pid $! --rig tests/smoke.luau --junit smoke.xml
```

`SIMXR_DETERMINISTIC=1` runs on simulated time so CI load doesn't change frame timing. For identical runs, pause the application and schedule input for exact frames, as `pose_sweep.luau` does. `simxr_runner` waits up to a minute for the application's OpenXR session and exits non-zero unless the rig completes. `--pid` only works if that process loads OpenXR itself, not through a launcher script.

### Checking rendered images

In a rig, `simxr.capture` and `simxr.export` save frames, and `simxr.export_history` saves frames the runtime already retained without pausing. To check images in the script itself, sample them (see [Sampling images](#sampling-images)); `examples/rigs/flicker_monitor.luau` detects flicker this way. For full-resolution analysis use the Python SDK, as in `examples/inspect-retained-image.py`.

### Starting the game from a known state

A game that links [`include/simxr/game_bridge.h`](../include/simxr/game_bridge.h) can take reset, step and custom commands from rigs. See `examples/rigs/game_reset.luau` and `examples/cooperating-game.py`.

### Debugging with an AI agent

Copy the configuration from **Preferences → MCP** into the agent's MCP client; the connector starts the companion if needed. The agent can launch and restart applications, take screenshots, read logs and diagnostics, pause and step frames, set input and run Luau.

## Reference

### Control endpoint

The runtime runs inside the application, and each application process listens on an endpoint named after its process ID: `$SIMXR_DATA_DIR/control.<pid>.sock` on Linux (default data directory `~/.local/share/OpenXR-Simulator`), or the pipe `\\.\pipe\SimXR.Control.<pid>` on Windows.

### Luau rigs

Run a rig from the companion's Library, with `simxr_runner (--pid ID | --socket PATH) --rig FILE [--seed N] [--deadline-ms N] [--json FILE] [--junit FILE]`, or with the MCP `simulator_luau_*` tools. Runs are limited to one hour and 256 printed lines.

Frame IDs and other 64-bit identifiers are decimal strings; use `simxr.frame_add` and `simxr.frame_before` instead of arithmetic. Units are metres and radians, and orientations are `{x, y, z, w}` quaternions. Input a rig takes stays held until `release_inputs` or the end of the rig. [`examples/rigs`](../examples/rigs) has a script for each area below.

| Area | `simxr.*` |
| --- | --- |
| Time and execution | `wait`, `monotonic_time`, `state`, `frame_add`, `frame_before`, `pause`, `resume`, `step`, `step_display`, `advance_to_frame` |
| Input | `schedule`, `await_transaction`, `release_inputs`, `start_input_recording`, `read_input_recording`, `stop_input_recording` |
| Images | `capture`, `export`, `release_capture`, `image_history`, `export_history`, `projection_history`, `*_image_recording`, `*_image_window`, `sample_*_image`, `inspect`, `revision`, `release_inspection` |
| Game bridge | `game_command`, `await_game`, `game_state` |
| Results | `checkpoint(name, passed, detail)`, `print` |

`schedule` accepts the commands of the MCP `simulation_schedule` tool.

### Sampling images

`image_history(count, {target = "composed"})` lists up to ten recent frames; `target = "quad_layers"` adds each frame's `quad_rectangles`. `sample_history_image(frame_id, options)` returns a small RGBA copy of a frame, or `nil` once it was evicted. `sample_window_image(window, frame_id, options)` does the same inside an image window.

A sample is `{pixels, width = 320, height = 90, eyes}`. `pixels` is an RGBA `buffer` with the left eye in the first 160 columns. Each eye is scaled to fit 160×90 and centred; `eyes[i]` gives the `x`, `y`, `width` and `height` it covers.

Pass `rectangles` to crop each eye in native pixels first. Regions up to 160×90 keep full resolution; a zero-sized region skips that eye.

```lua
local sample = simxr.sample_history_image(frame, {target = "composed", rectangles = {
    {x = 900, y = 500, width = 160, height = 90},
    {x = 880, y = 500, width = 160, height = 90}}})
local red = buffer.readu8(sample.pixels, (y * sample.width + x) * 4)
```

### Python SDK

```sh
pip install sdk/python  # share/simxr/sdk/python in a release
```

```python
from simxr_control import Client

with Client() as client:  # finds the only running endpoint, or pass its path
    session, = client.sessions()
    print(client.simulation_status(session)['document'])
```

### Time and the input queue

Every time in the control API is a predicted display time in nanoseconds, the `XrTime` the application sees. `schedule(time_ns=...)` applies at the first frame displayed at or after `time_ns`; `status()['pipeline']['waited']['time']` and `['period']` give the latest frame's display time and the frame period to count from, and `simulation_status` reports the display `time` of the last frame input was applied to.

A target more than 10000 frames or 60 seconds past the last applied frame is rejected with phase `too_far`, since it is almost always a value from another clock. `simulation_status` also reports the queue: `pending`, `pending_capacity`, the earliest `next_pending_frame` and `next_pending_time`, and `rejected`, which counts `late`, `conflict`, `invalid`, `capacity`, `too_far` and `closed` submissions and describes the `last` one.

### Moving the head

A `head_pose` with `linear_velocity` (metres per second) or `angular_velocity` (radians per second about the world axes) keeps the head moving from that pose on every frame, timed by display time, until the next `head_pose` or until the head is released. Steer live by sending a new pose and velocity whenever the direction changes:

```python
client.schedule(session, [{'kind': 'take_control', 'channel': 'head'},
                          {'kind': 'head_pose', 'position': [0, 1.7, 0], 'angular_velocity': [0, math.radians(12), 0]}],
                frame=next_frame)
```

### Pose sequences

A sequence is JSON lines, one row per keyframe, uploaded with `upload_sequence` and started with `start_sequence`, or both at once with `run_sequence`. A row may set `head` (`x`, `y`, `z` in metres, `yaw`, `pitch`, `roll` in radians), `left` and `right` (head-relative `posX`, `posY`, `posZ`, `yaw`, `pitch`, `roll`, `trigger`, `grip`, `stickX`, `stickY`, `primary`, `secondary`) and `capture`. Omitted values keep their current value.

Rows keyed by `frame` apply at that many frames after the sequence starts. Rows keyed by `time`, in seconds since the sequence's first frame, are evaluated on every frame: each value moves linearly between its own keyframes (controller orientation spherically), from its starting value before its first keyframe, and holds after its last. Buttons and captures fire on the first frame at or after their row. A looped time-keyed program repeats every last-row time. All rows of a program use one key. Motion keyed by time runs at the same speed at any frame rate:

```python
client.run_sequence(session, [{'time': 30, 'head': {'yaw': math.radians(360)}}])  # 12 degrees per second
```

`examples/timed-head-turn.py` turns the head this way.

A finished sequence holds its last values, and controllers it placed relative to the head keep following it. The client that started it takes those channels back with a plain `take_control`; other clients need `replace`.

### MCP

Start with `simulator_list_applications`; application tools take its `application_id`. `debug_connect` binds to one session and fails rather than follow a restart. Setters take a `transaction_id`, and repeating one returns the original receipt. IDs above 2^53−1 must be strings.

### Ownership

Each client owns the input, recordings and captures it starts, and releasing control or disconnecting cleans them up. Ending the OpenXR session cancels all automation for it. Image readers and exports stay valid until released.

### Runtime environment variables

Set these in the application's environment, or in a companion launch profile.

| Variable | Values |
| --- | --- |
| `SIMXR_DATA_DIR` | Directory for logs and, on Linux, control sockets |
| `SIMXR_HEADSET_PROFILE` | A name from `get_headset_profiles` |
| `SIMXR_WIDTH`, `SIMXR_HEIGHT` | Eye resolution |
| `SIMXR_DETERMINISTIC` | `1`: simulated time instead of wall-clock |
| `SIMXR_REFRESH_RATE` | Initial display refresh rate in Hz, 10 to 1000 (default 90). Apps can change it with `XR_FB_display_refresh_rate`, and the companion's headset page or a `views` command's `refresh_rate` overrides both |
| `SIMXR_APPLICATION_CADENCE` | `1`, `2` display frames per app frame, or `auto` |
| `SIMXR_PRESENTATION_POLICY` | `repeat` (default) or `rotational` reprojection of missed frames |
| `SIMXR_SEED` | Simulation seed, used by recorded rigs |
| `SIMXR_VULKAN_VALIDATION` | `1` |
| `SIMXR_CONTROL_TRACE`, `SIMXR_VERBOSE` | Request tracing (Linux), verbose logging (Windows) |
