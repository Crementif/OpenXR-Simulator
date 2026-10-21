"""CPU image transforms and analysis; callers schedule them off the event loop."""

import os
import re
from pathlib import Path
from typing import Any, Optional

SCREENSHOT_MAX_WIDTH = 1280
SCREENSHOT_JPEG_QUALITY = 70
STEREO_ANALYSIS_MAX_WIDTH = 1280


def to_jpeg(
    image_bytes: bytes,
    max_width: int = SCREENSHOT_MAX_WIDTH,
    quality: int = SCREENSHOT_JPEG_QUALITY,
    eye: str = "both",
) -> bytes:
    """Decode an image (PNG/BMP/JPEG) and re-encode as a downscaled JPEG.

    JPEG bounds the explicit image response after decoding native PNG/BMP files.
    """
    from io import BytesIO
    from PIL import Image

    with Image.open(BytesIO(image_bytes)) as source:
        # JPEG cannot store alpha; detach decoded pixels from the source stream.
        image = source.convert("RGB")
    # The D3D12 simulator capture path currently returns the full preview render
    # target regardless of the request's eye field. When the preview is SBS, crop
    # here so MCP callers still get the requested eye directly.
    eye = (eye or "both").lower()
    if os.name == "nt" and eye in ("left", "right") and image.width >= image.height * 2:
        half = image.width // 2
        if eye == "left":
            image = image.crop((0, 0, half, image.height))
        else:
            image = image.crop((half, 0, image.width, image.height))
    if image.width > max_width:
        scaled_height = int(image.height * (max_width / image.width))
        image = image.resize((max_width, scaled_height), Image.LANCZOS)
    out = BytesIO()
    image.save(out, format="JPEG", quality=quality, optimize=True)
    return out.getvalue()


def build_flicker_contact_sheet(
    incident_dir: Path, max_frames: int = 8
) -> Optional[bytes]:
    """Build an in-memory JPEG from simulator-composed preview frames for LLM review."""
    from io import BytesIO
    from PIL import Image, ImageDraw

    candidates = sorted(
        incident_dir.glob("frame*_preview.bmp"),
        key=lambda path: int(re.search(r"frame(\d+)", path.name).group(1)),
    )[-max(2, min(max_frames, 12)) :]
    if len(candidates) < 2:
        return None
    cells = []
    for path in candidates:
        resolved = path.resolve()
        resolved.relative_to(incident_dir.resolve())
        with Image.open(resolved) as source:
            image = source.convert("RGB")
        image.thumbnail((640, 360), Image.Resampling.LANCZOS)
        cell = Image.new("RGB", (image.width, image.height + 24), "#111318")
        cell.paste(image, (0, 24))
        ImageDraw.Draw(cell).text((6, 5), path.stem, fill="#f1f5f9")
        cells.append(cell)
    columns = 2
    cell_width = max(cell.width for cell in cells)
    cell_height = max(cell.height for cell in cells)
    rows = (len(cells) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * cell_width, rows * cell_height), "#090b10")
    for index, cell in enumerate(cells):
        sheet.paste(
            cell, ((index % columns) * cell_width, (index // columns) * cell_height)
        )
    output = BytesIO()
    sheet.save(output, format="JPEG", quality=78, optimize=True)
    return output.getvalue()


def validate_stereo_image(image_bytes: bytes) -> dict[str, Any]:
    """Crude horizontal-disparity check on a side-by-side stereo screenshot.

    We don't depend on OpenCV; just split the image down the middle and run
    a horizontal-shift template-match on a center-cropped window. Returns
    diagnostics (pixel disparity, recommended pass/fail thresholds, plus
    an IPD sanity estimate).
    """
    from io import BytesIO
    from PIL import Image

    with Image.open(BytesIO(image_bytes)) as source:
        image = source.convert("L")  # grayscale for matching

    # The preview is 2880x1584; a 121-step shift search over that takes long enough to
    # blow the MCP request timeout. Match on a downscaled copy and convert the measured
    # disparity back to full-resolution pixels at the end.
    original_width = image.size[0]
    if original_width > STEREO_ANALYSIS_MAX_WIDTH:
        scale = STEREO_ANALYSIS_MAX_WIDTH / original_width
        image = image.resize(
            (STEREO_ANALYSIS_MAX_WIDTH, max(1, round(image.size[1] * scale))),
            Image.BILINEAR,
        )
    else:
        scale = 1.0
    width, height = image.size
    # Heuristic: simulator preview is side-by-side at the top, but on
    # FORCE_STEREO games the simulator outputs the eye halves stacked.
    # Try both layouts and pick the one with the better correlation.
    half_width, half_height = width // 2, height // 2
    layouts = [
        (
            "side_by_side",
            image.crop((0, 0, half_width, height)),
            image.crop((half_width, 0, width, height)),
        ),
        (
            "over_under",
            image.crop((0, 0, width, half_height)),
            image.crop((0, half_height, width, height)),
        ),
    ]

    def _shift_search(left, right, max_shift):
        """Return (best_shift, score, contrast) where shift<0 means right shifted left."""
        left_eye = np.asarray(left, dtype=np.float32)
        right_eye = np.asarray(right, dtype=np.float32)
        # Crop a center window for matching to avoid edge artifacts.
        image_height, image_width = left_eye.shape
        window_height, window_width = image_height // 2, image_width // 2
        top_edge, bottom_edge = (image_height - window_height) // 2, (
            image_height - window_height
        ) // 2 + window_height
        left_edge, right_edge = (image_width - window_width) // 2, (
            image_width - window_width
        ) // 2 + window_width
        reference_window = left_eye[top_edge:bottom_edge, left_edge:right_edge]
        best_shift, best_score = 0, float("inf")
        # Smallest shift first, so a featureless window that matches equally well at
        # every offset reports 0 rather than whichever offset happened to come first.
        for shift in sorted(range(-max_shift, max_shift + 1), key=abs):
            sample_left, sample_right = left_edge + shift, right_edge + shift
            if sample_left < 0 or sample_right > image_width:
                continue
            candidate_window = right_eye[top_edge:bottom_edge, sample_left:sample_right]
            if candidate_window.shape != reference_window.shape:
                continue
            difference = np.mean((reference_window - candidate_window) ** 2)
            if difference < best_score:
                best_score, best_shift = difference, shift
        return best_shift, best_score, float(reference_window.std())

    try:
        import numpy as np
    except ImportError:
        return {"error": "numpy required for validate_stereo (pip install numpy)"}

    results = {}
    for name, left_eye, right_eye in layouts:
        if left_eye.size != right_eye.size:  # uneven crop — skip
            continue
        shift, score, contrast = _shift_search(
            left_eye, right_eye, max_shift=min(60, left_eye.size[0] // 6)
        )
        results[name] = {
            "disparity_px": shift,
            "score": float(score),
            "contrast": round(contrast, 2),
        }

    if not results:
        return {"error": "Could not split image into eye halves"}

    # Pick the layout with the most confident match. Comparing raw MSE would always
    # favour the flattest split -- halving a side-by-side preview the wrong way leaves
    # two featureless windows that match perfectly at any offset -- so only layouts
    # with actual detail in the match window are eligible, ranked by MSE relative to
    # that detail.
    MIN_CONTRAST = 2.0  # 8-bit levels of std dev
    usable = {
        name: match
        for name, match in results.items()
        if match["contrast"] >= MIN_CONTRAST
    }
    if not usable:
        return {
            "verdict": "INCONCLUSIVE_NO_FEATURES",
            "diagnosis": (
                "The match window is a flat area in both eyes, so no disparity can be "
                "measured. Capture a frame with visible geometry in the centre of view."
            ),
            "all_layouts": results,
        }
    best_layout = min(
        usable, key=lambda name: usable[name]["score"] / usable[name]["contrast"]
    )
    # Back to full-resolution pixels so the numbers mean the same thing regardless of
    # how far the analysis copy was downscaled.
    shift = round(results[best_layout]["disparity_px"] / scale)
    for entry in results.values():
        entry["disparity_px"] = round(entry["disparity_px"] / scale)
    eye_width = (
        (original_width // 2) if best_layout == "side_by_side" else original_width
    )

    # Heuristic verdict:
    #  - |shift| within [2, 30] px on a 1280-wide eye -> typical IPD parallax (PASS)
    #  - |shift| < 2  -> eyes nearly identical (FAIL: no IPD or aliased eyes)
    #  - |shift| > 30 -> excessive parallax (FAIL: IPD too large or wrong)
    expected_min = max(2, eye_width // 640)  # ~2 px on 1280-wide
    expected_max = max(30, eye_width // 40)  # ~30 px on 1280-wide
    if abs(shift) < expected_min:
        verdict = "FAIL_NO_PARALLAX"
        diagnosis = (
            "Both eye images look identical (disparity below noise floor). "
            "Likely cause: aliased per-eye matrix, IPD=0, or projection "
            "matrix not differentiating eyes."
        )
    elif abs(shift) > expected_max:
        verdict = "FAIL_EXCESSIVE_PARALLAX"
        diagnosis = (
            "Disparity is much larger than expected for typical IPD. "
            "Likely cause: IPD applied as raw OpenXR LOCAL-space position, "
            "or per-eye view matrix mis-translated."
        )
    else:
        verdict = "PASS"
        diagnosis = "Stereo disparity is within expected range for human IPD."

    return {
        "verdict": verdict,
        "diagnosis": diagnosis,
        "best_layout": best_layout,
        "horizontal_disparity_px": shift,
        "expected_range_px": [expected_min, expected_max],
        "all_layouts": results,
    }
