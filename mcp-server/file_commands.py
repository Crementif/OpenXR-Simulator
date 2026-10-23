"""Immutable compatibility file edits. Encoding never publishes or acknowledges."""

from dataclasses import dataclass
import json
import math
from tool_results import TextResult


@dataclass(frozen=True)
class FileEdit:
    filename: str
    document: bytes
    reply: str

    @classmethod
    def prepare(cls, filename, payload, reply):
        document = json.dumps(payload, allow_nan=False).encode("utf-8")
        if len(document) > 64 * 1024:
            raise ValueError("Compatibility command exceeds 64 KiB")
        return cls(filename, document, reply)


def _head(arguments):
    payload = {
        axis: float(arguments.get(axis, default))
        for axis, default in (("x", 0), ("y", 1.7), ("z", 0))
    }
    payload.update(
        {
            axis: math.radians(float(arguments.get(axis + "_deg", 0)))
            for axis in ("yaw", "pitch")
        }
    )
    if arguments.get("roll_deg") is not None:
        payload["roll"] = math.radians(float(arguments["roll_deg"]))
    return FileEdit.prepare(
        "head_pose_command.json", payload, f"Head pose command queued: {payload}"
    )


def _eye(arguments):
    if not arguments:
        return None
    return {
        key: math.radians(float(arguments.get(field, default)))
        for key, field, default in (
            ("aL", "left_deg", -45),
            ("aR", "right_deg", 45),
            ("aU", "up_deg", 45),
            ("aD", "down_deg", -45),
        )
    }


def _fov(arguments):
    if arguments.get("clear"):
        return FileEdit.prepare(
            "fov_command.json", {"clear": True}, "FOV reset to symmetric default queued."
        )
    left = _eye(arguments.get("left_eye"))
    right = _eye(arguments.get("right_eye"))
    if left is None or right is None:
        return TextResult(
            "set_fov requires both left_eye and right_eye sub-objects (or clear=true)"
        )
    return FileEdit.prepare(
        "fov_command.json",
        {"left": left, "right": right},
        f"Per-eye asymmetric FOV command queued. left={left} right={right}",
    )


def _ipd(arguments):
    if arguments.get("clear"):
        return FileEdit.prepare(
            "ipd_command.json", {"clear": True}, "IPD reset to 64 mm queued."
        )
    millimeters = float(arguments.get("ipd_mm", 64))
    if not 0 <= millimeters <= 200:
        return TextResult("ipd_mm must be in [0, 200]")
    return FileEdit.prepare(
        "ipd_command.json", {"ipd_mm": millimeters}, f"IPD command queued: {millimeters:.1f} mm"
    )


def _profile(arguments):
    name = arguments.get("name", "default")
    return FileEdit.prepare(
        "headset_profile_command.json",
        {"name": name},
        f"Headset profile command queued: {name}",
    )


def _anaglyph(arguments):
    enabled = bool(arguments.get("enabled", True))
    return FileEdit.prepare(
        "anaglyph_command.json",
        {"enabled": enabled},
        f"Anaglyph preview command queued: {'enabled' if enabled else 'disabled'}.",
    )


def _pose_sweep(arguments):
    payload = {
        "enabled": bool(arguments.get("enabled", True)),
        **{
            key: float(arguments.get(key, default))
            for key, default in (
                ("yaw_amp_deg", 30),
                ("pitch_amp_deg", 15),
                ("roll_amp_deg", 15),
                ("freq_hz", 0.25),
            )
        },
    }
    reply = (
        "Pose sweep command queued: "
        + ("enabled" if payload["enabled"] else "disabled")
        + f" — yaw=±{payload['yaw_amp_deg']}° pitch=±{payload['pitch_amp_deg']}°"
        f" roll=±{payload['roll_amp_deg']}° freq={payload['freq_hz']}Hz"
    )
    return FileEdit.prepare("pose_sweep_command.json", payload, reply)


def _wheel_sweep(arguments):
    enabled = arguments.get("enabled", True)
    payload = {"enabled": 1 if enabled else 0}
    if not enabled:
        return FileEdit.prepare(
            "wheel_sweep_command.json",
            payload,
            "Wheel sweep stop and controller release queued.",
        )
    fields = (
        ("hubX", "hub_x", 0),
        ("hubY", "hub_y", -0.35),
        ("hubZ", "hub_z", -0.45),
        ("radius", "radius", 0.18),
        ("tiltDeg", "tilt_deg", 20),
        ("amplitudeDeg", "amplitude_deg", 80),
        ("freqHz", "freq_hz", 0.15),
        ("grip", "grip", 1),
        ("spreadDeg", "spread_deg", 90),
    )
    payload.update(
        {
            field: float(arguments.get(argument, default))
            for field, argument, default in fields
        }
    )
    period = 1 / payload["freqHz"] if payload["freqHz"] else 0
    reply = (
        f"Wheel sweep command queued: +/-{payload['amplitudeDeg']:.0f} deg over a {period:.2f}s period, "
        f"rim radius {payload['radius']:.3f} m raked {payload['tiltDeg']:.0f} deg, "
        f"hub ({payload['hubX']:.2f}, {payload['hubY']:.2f}, {payload['hubZ']:.2f}) head-local, "
        f"grip held at {payload['grip']:.2f}."
    )
    return FileEdit.prepare("wheel_sweep_command.json", payload, reply)


_ENCODERS = {
    "set_head_pose": _head,
    "set_fov": _fov,
    "set_ipd": _ipd,
    "set_headset_profile": _profile,
    "enable_anaglyph_preview": _anaglyph,
    "enable_pose_sweep": _pose_sweep,
    "enable_wheel_sweep": _wheel_sweep,
}
_LINUX_FILES = {
    "set_controller_pose": "controller_pose_command.json",
    "hold_controller_inputs": "controller_hold_command.json",
    "run_pose_sequence": "sequence_command.json",
}


def encode_file_edit(name, arguments, platform):
    encoder = _ENCODERS.get(name)
    if encoder is not None:
        return encoder(arguments)
    if platform != "nt" and name in _LINUX_FILES:
        return FileEdit.prepare(
            _LINUX_FILES[name],
            arguments,
            f"Linux simulator command queued: {arguments}",
        )
    return None
