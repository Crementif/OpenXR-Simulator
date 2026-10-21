"""Immutable tool descriptions; each caller receives its own schema values."""

import copy
import os
from debug_control import TOOL_DEFINITIONS


def tool_definitions(platform=None):
    """List available tools without sharing mutable schemas across callers."""
    platform = os.name if platform is None else platform
    tools = [
        dict(
            name="capture_screenshot",
            description="Capture a screenshot of the current OpenXR frame being rendered. "
            "Returns the stereo view (left and right eye) as displayed in the preview window.",
            inputSchema={
                "type": "object",
                "properties": {
                    "eye": {
                        "type": "string",
                        "enum": ["both", "left", "right"],
                        "default": "both",
                        "description": "Which eye view to capture",
                    },
                    "layer": {
                        "type": "string",
                        "enum": ["projection", "quad", "all"],
                        "default": "all",
                        "description": "Which submitted layer types to capture",
                    },
                    "timeout": {
                        "type": "number",
                        "default": 5.0,
                        "description": "Timeout in seconds to wait for screenshot",
                    },
                },
            },
        ),
        dict(
            name="get_frame_info",
            description="Get detailed information about the current frame including timing, "
            "resolution, format, head tracking state, and session state.",
            inputSchema={"type": "object", "properties": {}},
        ),
        dict(
            name="get_flicker_status",
            description="Read the continuous detector attached to the OpenXR Simulator's final composed preview. "
            "Returns layer-continuity and pixel anomaly counters and, when available, a contact sheet "
            "from the latest incident for direct LLM visual review.",
            inputSchema={
                "type": "object",
                "properties": {
                    "include_images": {
                        "type": "boolean",
                        "default": True,
                        "description": "Include an image contact sheet from the latest incident",
                    },
                    "max_frames": {
                        "type": "integer",
                        "default": 8,
                        "minimum": 2,
                        "maximum": 12,
                    },
                },
            },
        ),
        dict(
            name="capture_flicker_window",
            description="Capture the simulator's rolling composed-preview history plus following frames now, "
            "even if the automatic detector threshold has not fired. Returns a contact sheet for "
            "direct temporal review by an LLM.",
            inputSchema={
                "type": "object",
                "properties": {
                    "timeout": {
                        "type": "number",
                        "default": 5.0,
                        "minimum": 1.0,
                        "maximum": 15.0,
                    },
                    "max_frames": {
                        "type": "integer",
                        "default": 12,
                        "minimum": 2,
                        "maximum": 12,
                    },
                },
            },
        ),
        dict(
            name="get_ui_flicker_status",
            description="Read UI-only quad-layer flicker diagnostics. This ignores world motion and reports "
            "whether every new projection containing a submitted UI quad was actually recomposed, "
            "plus cropped UI temporal evidence for the latest incident.",
            inputSchema={
                "type": "object",
                "properties": {
                    "include_images": {"type": "boolean", "default": True},
                    "max_frames": {
                        "type": "integer",
                        "default": 8,
                        "minimum": 2,
                        "maximum": 12,
                    },
                },
            },
        ),
        dict(
            name="capture_ui_flicker_window",
            description="Force a rolling pre/post capture cropped to the left/right UI quad rectangles. "
            "Use this when a person reports UI flicker but whole-frame detection is clean.",
            inputSchema={
                "type": "object",
                "properties": {
                    "timeout": {
                        "type": "number",
                        "default": 5.0,
                        "minimum": 1.0,
                        "maximum": 15.0,
                    },
                    "max_frames": {
                        "type": "integer",
                        "default": 12,
                        "minimum": 2,
                        "maximum": 12,
                    },
                },
            },
        ),
        dict(
            name="read_logs",
            description="Read recent entries from the OpenXR Simulator log file. "
            "Useful for debugging issues and understanding runtime behavior.",
            inputSchema={
                "type": "object",
                "properties": {
                    "lines": {
                        "type": "integer",
                        "default": 100,
                        "description": "Number of recent log lines to read",
                    },
                    "filter": {
                        "type": "string",
                        "description": "Optional regex pattern to filter log lines",
                    },
                },
            },
        ),
        dict(
            name="get_diagnostics",
            description="Get comprehensive diagnostic information about the OpenXR Simulator "
            "including session state, frame count, swapchain info, and any errors.",
            inputSchema={"type": "object", "properties": {}},
        ),
        dict(
            name="diagnose_issue",
            description="Analyze potential OpenXR issues based on described symptoms. "
            "Provides potential causes and recommendations based on log analysis.",
            inputSchema={
                "type": "object",
                "properties": {
                    "symptoms": {
                        "type": "string",
                        "description": "Description of the issue or symptoms you're experiencing",
                    }
                },
                "required": ["symptoms"],
            },
        ),
        dict(
            name="get_session_state",
            description="Get the current OpenXR session state (IDLE, READY, SYNCHRONIZED, "
            "VISIBLE, FOCUSED, STOPPING, EXITING).",
            inputSchema={"type": "object", "properties": {}},
        ),
        dict(
            name="get_head_tracking",
            description="Get the current head tracking state including position and orientation.",
            inputSchema={"type": "object", "properties": {}},
        ),
        dict(
            name="clear_logs",
            description="Clear the OpenXR Simulator log file to start fresh.",
            inputSchema={"type": "object", "properties": {}},
        ),
        dict(
            name="enable_wheel_sweep",
            description=(
                "Put BOTH controllers on a virtual steering wheel's RIM and rotate "
                "them together on a sine, with the grip held. A one-shot controller "
                "pose cannot test a VR steering wheel: the motion has to be "
                "continuous and it has to be the SAME motion twice. This waveform is "
                "generated inside the runtime, so it is a closed-form function of the "
                "clock and two runs of the same command replay identically. Set the "
                "hub/radius/tilt to the app's OWN wheel geometry so the hands land on "
                'its rim rather than near it. Pass {"enabled": false} to stop and '
                "hand the controllers back."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "enabled": {"type": "boolean", "default": True},
                    "amplitude_deg": {
                        "type": "number",
                        "default": 80.0,
                        "description": "Peak wheel rotation each way",
                    },
                    "freq_hz": {
                        "type": "number",
                        "default": 0.15,
                        "description": "Sine frequency. Keep the PERIOD incommensurate with the interval of whatever log you are reading, or every sample lands at the same phase and the hook looks dead.",
                    },
                    "radius": {
                        "type": "number",
                        "default": 0.18,
                        "description": "Rim radius in metres",
                    },
                    "tilt_deg": {
                        "type": "number",
                        "default": 20.0,
                        "description": "Rake: 0 is a vertical wheel, 90 a flat one",
                    },
                    "hub_x": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Hub offset from the head, head-local metres",
                    },
                    "hub_y": {
                        "type": "number",
                        "default": -0.35,
                        "description": "Negative is below the head",
                    },
                    "hub_z": {
                        "type": "number",
                        "default": -0.45,
                        "description": "Negative is in front of the head",
                    },
                    "grip": {
                        "type": "number",
                        "default": 1.0,
                        "description": "Squeeze axis value held while enabled; 0 releases both hands",
                    },
                    "spread_deg": {
                        "type": "number",
                        "default": 90.0,
                        "description": "Hands at -/+ this bearing from the top: 90 is 9 and 3 o'clock",
                    },
                },
            },
        ),
        dict(
            name="set_head_pose",
            description=(
                "Set the simulator's head pose. Use a non-identity pose "
                "(yaw/pitch/roll != 0) to surface coordinate-system "
                "/ quaternion-handedness bugs that an identity head "
                "pose would hide. Roll is especially valuable for "
                "catching axis-flip bugs."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "x": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Head X position (meters)",
                    },
                    "y": {
                        "type": "number",
                        "default": 1.7,
                        "description": "Head Y position (meters)",
                    },
                    "z": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Head Z position (meters)",
                    },
                    "yaw_deg": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Yaw in degrees (left/right)",
                    },
                    "pitch_deg": {
                        "type": "number",
                        "default": 0.0,
                        "description": "Pitch in degrees (up/down)",
                    },
                    "roll_deg": {
                        "type": "number",
                        "description": "Roll in degrees (head tilt). Omit to leave unchanged.",
                    },
                },
            },
        ),
        dict(
            name="set_fov",
            description=(
                "Override the per-eye OpenXR FOV with asymmetric values "
                "(degrees). The simulator's default is symmetric, which "
                "hides projection-matrix bugs that show up against a real "
                'headset\'s asymmetric lens FOV. Pass {"clear": true} to '
                "revert to the symmetric default."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "clear": {"type": "boolean", "default": False},
                    "left_eye": {
                        "type": "object",
                        "description": "Left eye FOV (degrees, +/- around forward)",
                        "properties": {
                            "left_deg": {"type": "number"},
                            "right_deg": {"type": "number"},
                            "up_deg": {"type": "number"},
                            "down_deg": {"type": "number"},
                        },
                    },
                    "right_eye": {
                        "type": "object",
                        "description": "Right eye FOV (degrees, +/- around forward)",
                        "properties": {
                            "left_deg": {"type": "number"},
                            "right_deg": {"type": "number"},
                            "up_deg": {"type": "number"},
                            "down_deg": {"type": "number"},
                        },
                    },
                },
            },
        ),
        dict(
            name="set_ipd",
            description=(
                "Override the per-eye separation (interpupillary distance). "
                "Set to 0 to verify the app responds to no-parallax (both eyes "
                "should render identical images). Set to 80 mm to check large-IPD "
                'behavior. Pass {"clear": true} to revert to 64 mm.'
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "ipd_mm": {
                        "type": "number",
                        "description": "IPD in millimeters (0-100)",
                    },
                    "clear": {"type": "boolean", "default": False},
                },
            },
        ),
        dict(
            name="set_headset_profile",
            description=(
                "Apply a named headset preset (FOV + IPD together), using "
                "the per-eye frustum each headset's runtime actually "
                "reports. Useful for quickly testing whether the app "
                "handles asymmetric lens profiles correctly. Available: "
                "quest2, quest3, questpro, index, vivepro2, reverbg2, "
                "psvr2, pico4, beyond, generic/default (revert)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "enum": [
                            "quest2",
                            "quest3",
                            "questpro",
                            "index",
                            "vivepro2",
                            "reverbg2",
                            "psvr2",
                            "pico4",
                            "beyond",
                            "generic",
                            "default",
                            "clear",
                        ],
                    }
                },
                "required": ["name"],
            },
        ),
        dict(
            name="enable_anaglyph_preview",
            description=(
                "Toggle the simulator's anaglyph (red/cyan) stereo preview "
                "overlay. When enabled, the preview window composites left "
                "and right eyes into a single red/cyan image. Properly "
                "converged stereo will appear (mostly) gray; broken IPD or "
                "swapped eyes will show pronounced red/cyan ghosting."
            ),
            inputSchema={
                "type": "object",
                "properties": {"enabled": {"type": "boolean", "default": True}},
            },
        ),
        dict(
            name="get_projection_log",
            description=(
                "Returns the recent projection-layer FOV / pose / image-rect "
                "the app submitted via xrEndFrame. Lets you compare what the "
                "app told the compositor against what the simulator was "
                "configured to publish — a quick way to find FOV mismatch."
            ),
            inputSchema={
                "type": "object",
                "properties": {"max_entries": {"type": "integer", "minimum": 1, "maximum": 64, "default": 20}},
            },
        ),
        dict(
            name="enable_pose_sweep",
            description=(
                "Auto-oscillate the simulator's head yaw / pitch / roll "
                "on a sine wave. Forces the app through a continuous "
                "range of orientations — any quaternion-handedness or "
                "axis-flip bug produces a visible 'world wobbles wrong "
                "direction' symptom within seconds. Each axis runs at "
                "a different phase so the motion isn't degenerate."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "enabled": {"type": "boolean", "default": True},
                    "yaw_amp_deg": {"type": "number", "default": 30.0},
                    "pitch_amp_deg": {"type": "number", "default": 15.0},
                    "roll_amp_deg": {"type": "number", "default": 15.0},
                    "freq_hz": {"type": "number", "default": 0.25},
                },
            },
        ),
        dict(
            name="analyze_stereo_consistency",
            description=(
                "Find where the two eyes DISAGREE about the world: shadows, lighting or "
                "effects that one eye sees and the other does not, or that are drawn at "
                "a different depth from the surface they lie on. Matches the eyes on "
                "illumination-tolerant features (semi-global), warps one onto the other "
                "and classifies what is left as ONE_EYE_ONLY, WRONG_DEPTH or "
                "SCREEN_LOCKED (no parallax). Also checks the measured far disparity "
                "against the frusta the app declared, vertical offset and per-eye "
                "exposure. Returns a report plus a sheet (both eyes with the regions "
                "boxed, the residual map, the disparity map). With frames > 1 it samples "
                "repeatedly and returns the worst frame, for use while the scene moves."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "frames": {
                        "type": "integer",
                        "default": 1,
                        "minimum": 1,
                        "maximum": 30,
                        "description": "How many frames to sample",
                    },
                    "interval": {
                        "type": "number",
                        "default": 1.0,
                        "description": "Seconds between samples when frames > 1",
                    },
                    "threshold": {
                        "type": "number",
                        "default": 0.16,
                        "description": "Log-luminance difference that counts as rivalry (0.16 is ~17%)",
                    },
                    "include_image": {"type": "boolean", "default": True},
                    "timeout": {"type": "number", "default": 5.0},
                },
            },
        ),
        dict(
            name="validate_stereo",
            description=(
                "Capture the current stereo preview and run a disparity "
                "analysis. Returns: verdict (PASS / FAIL_NO_PARALLAX / "
                "FAIL_EXCESSIVE_PARALLAX / INCONCLUSIVE_NO_FEATURES when the "
                "centre of view is a flat area), measured horizontal pixel "
                "disparity, expected range, and a diagnostic hint."
            ),
            inputSchema={
                "type": "object",
                "properties": {"timeout": {"type": "number", "default": 5.0}},
            },
        ),
    ]
    if platform != "nt":
        windows_only = {
            "enable_wheel_sweep",
            "set_headset_profile",
            "enable_anaglyph_preview",
            "enable_pose_sweep",
        }
        tools = [tool for tool in tools if tool["name"] not in windows_only]
        tools.extend(
            [
                dict(
                    name="set_controller_pose",
                    description="Set a Linux simulator controller pose in head-local metres/radians. hand: 0 left, 1 right.",
                    inputSchema={
                        "type": "object",
                        "properties": {
                            key: {"type": "number"}
                            for key in (
                                "hand",
                                "posX",
                                "posY",
                                "posZ",
                                "yaw",
                                "pitch",
                                "roll",
                                "trigger",
                            )
                        },
                    },
                ),
                dict(
                    name="hold_controller_inputs",
                    description="Hold Linux controller inputs until explicitly changed or cleared. hand: 0 left, 1 right, -1 both.",
                    inputSchema={
                        "type": "object",
                        "properties": {
                            "clear": {"type": "boolean"},
                            **{
                                key: {"type": "number"}
                                for key in (
                                    "hand",
                                    "trigger",
                                    "grip",
                                    "primary",
                                    "secondary",
                                    "menu",
                                    "thumbClick",
                                    "stickX",
                                    "stickY",
                                )
                            },
                        },
                    },
                ),
                dict(
                    name="run_pose_sequence",
                    description="Replay frame-indexed Linux simulator pose/action JSONL from an absolute path. Each line may contain frame, head, left, right and capture.",
                    inputSchema={
                        "type": "object",
                        "properties": {
                            "path": {"type": "string"},
                            "loop": {"type": "boolean", "default": False},
                        },
                        "required": ["path"],
                    },
                ),
            ]
        )
    tools.extend(copy.deepcopy(TOOL_DEFINITIONS))
    return tools
