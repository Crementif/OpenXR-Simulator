"""Log/status interpretation scoped to one artifact owner and selected log snapshot."""

from datetime import datetime
import json
import math
import os
import re
import time
from typing import Any
from runtime_artifacts import Artifact
from tool_results import TextResult


class RuntimeDiagnostics:
    def __init__(self, artifacts):
        self._artifacts = artifacts

    def runtime_live(self):
        log = self._artifacts.log_snapshot()
        return log.live if log else False

    def status_age(self):
        try:
            return round(time.time() - self._artifacts.modified(Artifact.STATUS), 1)
        except OSError:
            return None

    def read_logs(self, lines=100, filter_pattern=None):
        snapshot = self._artifacts.log_snapshot()
        if snapshot is None:
            return "Log file not found. The OpenXR Simulator may not have been run yet."
        try:
            recent = snapshot.text.splitlines(keepends=True)
            if len(recent) > lines:
                recent = recent[-lines:]
            if filter_pattern:
                expression = re.compile(filter_pattern, re.IGNORECASE)
                recent = [line for line in recent if expression.search(line)]
            return "".join(recent)
        except Exception as error:
            return f"Error reading log file: {error}"

    def diagnostics(self) -> dict[str, Any]:
        """Parse the log file to extract diagnostic information."""
        diagnostics = {
            "session_state": "Unknown",
            "frame_count": 0,
            "swapchain_info": [],
            "errors": [],
            "warnings": [],
            "last_activity": None,
            "graphics_api": "Unknown",
            "head_tracking": {"position": None, "orientation": None},
            "runtime_live": False,
            "log_file": None,
        }

        log_file = self._artifacts.log_snapshot()
        if log_file is None:
            return diagnostics
        diagnostics["log_file"] = log_file.path.name
        diagnostics["runtime_live"] = log_file.live
        if log_file.truncated:
            diagnostics["log_truncated"] = True

        try:
            content = log_file.text

            # Extract session state. The runtime logs transitions as
            # "PushState: Session <handle> -> FOCUSED"; the second pattern is the legacy form.
            state_matches = re.findall(
                r"PushState: Session \d+ -> (\w+)", content
            ) or re.findall(r"session state -> (\w+)", content)
            if state_matches:
                diagnostics["session_state"] = state_matches[-1]

            frame_matches = re.findall(r"xrEndFrame called \(frame #(\d+)\)", content)
            if frame_matches:
                diagnostics["frame_count"] = int(frame_matches[-1])

            swapchain_matches = re.findall(
                r"xrCreateSwapchain.*?format=(\w+).*?(\d+)x(\d+)", content
            )
            for match in swapchain_matches:
                diagnostics["swapchain_info"].append(
                    {
                        "format": match[0],
                        "width": int(match[1]),
                        "height": int(match[2]),
                    }
                )

            error_matches = re.findall(r"\[SimXR\].*?ERROR.*", content, re.IGNORECASE)
            diagnostics["errors"] = error_matches[-10:]  # Last 10 errors

            warning_matches = re.findall(
                r"\[SimXR\].*?WARNING.*", content, re.IGNORECASE
            )
            diagnostics["warnings"] = warning_matches[-10:]  # Last 10 warnings

            # Extract graphics API from the binding the session actually chose. A plain
            # substring test always says D3D12, because the extension list names every API.
            api_match = re.findall(r"xrCreateSession: SUCCESS \((\w+)", content)
            if api_match:
                diagnostics["graphics_api"] = api_match[-1]

            diagnostics["last_activity"] = datetime.fromtimestamp(
                log_file.modified
            ).isoformat()

            # Head pose lives in the status file, not the log. Both sources throttle their
            # frame counter (the log stops at 10 then logs every 60th, the status file
            # rewrites every 30th), so the larger of the two is the closer estimate.
            status = self._artifacts.read_json(Artifact.STATUS)
            if status:
                tracking = status.get("head_tracking") or {}
                if tracking.get("position") is not None:
                    diagnostics["head_tracking"] = tracking
                diagnostics["frame_count"] = max(
                    diagnostics["frame_count"], int(status.get("frame_count", 0))
                )

        except Exception as error:
            diagnostics["parse_error"] = str(error)

        return diagnostics

    def frame_info(self) -> dict[str, Any]:
        """Get current frame information from the runtime status file."""
        status = self._artifacts.read_json(Artifact.STATUS)
        if status is None:
            if self._artifacts.exists(Artifact.STATUS):
                return {"error": "Failed to read runtime_status.json"}
            return self.diagnostics()

        # The runtime leaves the last frame's status behind when it exits, so a caller
        # that just reads session_state would see FOCUSED long after the app is gone.
        status["runtime_live"] = self.runtime_live()
        status["status_age_seconds"] = self.status_age()
        if not status["runtime_live"]:
            status["stale"] = (
                "No process currently has the simulator runtime loaded; this is the last frame of a finished session."
            )
        return status

    def analyze(self, symptoms: str) -> dict[str, Any]:
        """Analyze potential OpenXR issues based on symptoms and log data."""
        diagnostics = self.diagnostics()
        logs = self.read_logs(lines=200)

        analysis = {
            "symptoms": symptoms,
            "diagnostics": diagnostics,
            "potential_issues": [],
            "recommendations": [],
        }

        symptoms_lower = symptoms.lower()

        if "black" in symptoms_lower or "blank" in symptoms_lower:
            analysis["potential_issues"].append("Blank/black screen in VR view")
            if diagnostics["frame_count"] == 0:
                analysis["recommendations"].append(
                    "No frames have been submitted. Check if xrEndFrame is being called."
                )
            if not diagnostics["swapchain_info"]:
                analysis["recommendations"].append(
                    "No swapchains created. Verify xrCreateSwapchain is successful."
                )
            analysis["recommendations"].append(
                "Check that projection layers are being submitted in xrEndFrame."
            )

        if "crash" in symptoms_lower or "error" in symptoms_lower:
            analysis["potential_issues"].append("Runtime errors or crashes")
            if diagnostics["errors"]:
                analysis["recommendations"].append(
                    f"Found {len(diagnostics['errors'])} errors in log. Review them below."
                )
                analysis["recent_errors"] = diagnostics["errors"]

        if "tracking" in symptoms_lower or "position" in symptoms_lower:
            analysis["potential_issues"].append("Head tracking issues")
            analysis["recommendations"].append(
                "The simulator uses mouse for head rotation and WASD for movement. "
                "Click the preview window to capture mouse."
            )

        if (
            "performance" in symptoms_lower
            or "lag" in symptoms_lower
            or "slow" in symptoms_lower
        ):
            analysis["potential_issues"].append("Performance issues")
            analysis["recommendations"].append(
                "Check frame timing - the simulator targets 90Hz (11.1ms per frame)."
            )
            if diagnostics["graphics_api"] == "D3D12":
                analysis["recommendations"].append(
                    "D3D12 path is in use. Complex command list management may cause overhead."
                )

        if "format" in symptoms_lower or "color" in symptoms_lower:
            analysis["potential_issues"].append("Texture format issues")
            if diagnostics["swapchain_info"]:
                analysis["recommendations"].append(
                    f"Swapchain formats in use: {[s['format'] for s in diagnostics['swapchain_info']]}"
                )
            analysis["recommendations"].append(
                "Verify sRGB vs linear color space handling is correct."
            )

        if "error" in logs.lower() or "warning" in logs.lower():
            analysis["log_excerpt"] = self.read_logs(
                lines=50, filter_pattern=r"(error|warning|fail)"
            )

        return analysis

    def frame(self, arguments):
        info = self.frame_info()
        return [TextResult(text=json.dumps(info, indent=2))]

    def logs(self, arguments):
        lines = arguments.get("lines", 100)
        filter_pattern = arguments.get("filter")
        logs = self.read_logs(lines=lines, filter_pattern=filter_pattern)
        return [
            TextResult(
                text=f"=== OpenXR Simulator Logs (last {lines} lines) ===\n\n{logs}"
            )
        ]

    def summary(self, arguments):
        diagnostics = self.diagnostics()
        return [TextResult(text=json.dumps(diagnostics, indent=2))]

    def issue(self, arguments):
        symptoms = arguments.get("symptoms", "")
        analysis = self.analyze(symptoms)

        output = ["=== OpenXR Issue Analysis ===\n"]
        output.append(f"Symptoms: {analysis['symptoms']}\n")

        if analysis["potential_issues"]:
            output.append("\nPotential Issues:")
            for issue in analysis["potential_issues"]:
                output.append(f"  - {issue}")

        if analysis["recommendations"]:
            output.append("\nRecommendations:")
            for recommendation in analysis["recommendations"]:
                output.append(f"  - {recommendation}")

        if "recent_errors" in analysis:
            output.append("\nRecent Errors from Log:")
            for error in analysis["recent_errors"]:
                output.append(f"  {error.strip()}")

        if "log_excerpt" in analysis:
            output.append(f"\nRelevant Log Entries:\n{analysis['log_excerpt']}")

        output.append("\n\nDiagnostics Summary:")
        output.append(json.dumps(analysis["diagnostics"], indent=2))

        return [TextResult(text="\n".join(output))]

    def session(self, arguments):
        diagnostics = self.diagnostics()
        status = self._artifacts.read_json(Artifact.STATUS) or {}
        if diagnostics.get("runtime_live"):
            state = status.get("session_state") or diagnostics.get(
                "session_state", "Unknown"
            )
        else:
            state = diagnostics.get("session_state") or status.get(
                "session_state", "Unknown"
            )

        state_descriptions = {
            "IDLE": "Session created but not yet started",
            "READY": "Session ready to begin rendering",
            "SYNCHRONIZED": "Session synchronized with display",
            "VISIBLE": "Application is visible but not focused",
            "FOCUSED": "Application has focus and full input",
            "STOPPING": "Session is stopping",
            "EXITING": "Session is exiting",
            "Unknown": "State unknown - check if runtime is active",
        }

        text = f"Session State: {state}\n\n{state_descriptions.get(state, '')}"
        if state != "Unknown" and not diagnostics.get("runtime_live"):
            age = self.status_age()
            age_note = f" (last updated {age:.0f}s ago)" if age is not None else ""
            text += f"\n\nNOTE: no process currently has the runtime loaded -- this is the final state of a finished session{age_note}."
        return [TextResult(text=text)]

    def head(self, arguments):
        info = self.frame_info()
        tracking = info.get("head_tracking", {})

        if not tracking or tracking.get("position") is None:
            logs = self.read_logs(lines=50)
            pos_match = re.search(
                r"head pos.*?(-?\d+\.?\d*),\s*(-?\d+\.?\d*),\s*(-?\d+\.?\d*)",
                logs,
                re.IGNORECASE,
            )
            if pos_match:
                tracking["position"] = {
                    "x": float(pos_match.group(1)),
                    "y": float(pos_match.group(2)),
                    "z": float(pos_match.group(3)),
                }

        output = "=== Head Tracking State ===\n\n"
        if tracking.get("position"):
            position = tracking["position"]
            output += f"Position: ({position.get('x', 0):.3f}, {position.get('y', 0):.3f}, {position.get('z', 0):.3f})\n"
        else:
            output += "Position: Default (0.0, 1.7, 0.0)\n"

        # The runtime reports orientation as Euler angles in radians; only fall back to
        # "identity" when it published no angles at all.
        angles = {
            angle: tracking[angle]
            for angle in ("yaw", "pitch", "roll")
            if tracking.get(angle) is not None
        }
        if tracking.get("orientation"):
            orientation = tracking["orientation"]
            output += f"Orientation (quaternion): ({orientation.get('x', 0):.3f}, {orientation.get('y', 0):.3f}, {orientation.get('z', 0):.3f}, {orientation.get('w', 1):.3f})\n"
        elif angles:
            output += (
                "Orientation: "
                + ", ".join(
                    f"{angle} {math.degrees(radians):.1f} deg"
                    for angle, radians in angles.items()
                )
                + "\n"
            )
        else:
            output += "Orientation: Default (identity)\n"

        if not info.get("runtime_live", True):
            output += "\nNOTE: no process currently has the runtime loaded -- these are the last values of a finished session.\n"

        if os.name == "nt":
            output += "\nControls:\n"
            output += "  - Mouse: Look around (click preview window to capture)\n"
            output += "  - WASD: Move forward/left/backward/right\n"
            output += "  - ESC: Release mouse capture\n"
        else:
            output += "\nUse set_head_pose, set_controller_pose, hold_controller_inputs or run_pose_sequence to control Linux tracking/input.\n"

        return [TextResult(text=output)]

    def clear(self, arguments):
        try:
            # One log per process, and the live runtime holds its own open -- delete what
            # we can and name what we could not.
            removed, held = self._artifacts.clear_logs()
            text = (
                f"Cleared {len(removed)} log file(s)."
                if removed
                else "No log files to clear."
            )
            if held:
                text += f" Still in use by a running process: {', '.join(held)}."
            return [TextResult(text=text)]
        except Exception as error:
            return [TextResult(text=f"Failed to clear log file: {error}")]
