"""Synthetic stereo scenes with independently constructed disparity and lighting faults."""

import numpy as np
from PIL import Image

W, H = 640, 400
HORIZON = 120
INFINITY = 40  # px: asymmetric frusta put infinity at a constant offset, as a headset does


def _texture(seed: int, shape) -> np.ndarray:
    """Band-limited colour noise: detail at every scale a matcher could want."""
    rng = np.random.default_rng(seed)
    out = np.zeros(shape + (3,), np.float32)
    for cell, gain in ((48, 0.20), (16, 0.16), (6, 0.12), (2, 0.08)):
        small = rng.random((shape[0] // cell + 2, shape[1] // cell + 2, 3), dtype=np.float32)
        big = np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize(
            (shape[1], shape[0]), Image.BICUBIC), np.float32) / 255.0
        out += (big - 0.5) * gain
    return out


def _scene():
    """World-anchored colour and the disparity of every left-eye pixel."""
    ys = np.arange(H, dtype=np.float32)[:, None].repeat(W, 1)
    xs = np.arange(W, dtype=np.float32)[None, :].repeat(H, 0)
    # Ground: disparity grows linearly below the horizon. Sky/backdrop: at infinity.
    disparity = INFINITY + np.clip(ys - HORIZON, 0, None) * 0.11
    box = (xs > 250) & (xs < 360) & (ys > 150) & (ys < 250)
    disparity = np.where(box, INFINITY + 26.0, disparity)
    return disparity.astype(np.float32), box


def render(shadow=None):
    """Returns (left, right) uint8 images. `shadow(eye, x_world, y)` -> multiplier map."""
    disparity, box = _scene()
    # Colour lives in "cyclopean surface" coordinates: x + disparity/2 is the same surface
    # point for both eyes, so sampling the texture there is what makes it world-anchored.
    wide = W + 200
    ground = 0.45 + _texture(1, (H, wide))
    ground[..., 2] -= 0.08
    crate = 0.55 + _texture(2, (H, wide))
    crate[..., 0] += 0.12
    ys = np.arange(H)[:, None].repeat(W, 1)
    xs = np.arange(W, dtype=np.float32)[None, :].repeat(H, 0)

    images = []
    for eye in ("left", "right"):
        # A left-eye pixel x shows surface point s = x - d/2; the right eye sees the same
        # point at x - d, i.e. a right-eye pixel x shows s = x + d/2 (d taken at its source).
        # Build the right eye by forward-mapping the left eye's disparity.
        if eye == "left":
            d = disparity
            is_box = box
        else:
            d = np.full((H, W), -1.0, np.float32)
            is_box = np.zeros((H, W), bool)
            for layer in (~box, box):  # far first, near overwrites: correct occlusion
                yy, xx = np.nonzero(layer)
                tx = np.round(xx - disparity[yy, xx]).astype(int)
                ok = (tx >= 0) & (tx < W)
                d[yy[ok], tx[ok]] = disparity[yy[ok], xx[ok]]
                is_box[yy[ok], tx[ok]] = layer is box
            # Holes (disocclusions) take the background disparity of their row.
            row = INFINITY + np.clip(np.arange(H, dtype=np.float32) - HORIZON, 0, None) * 0.11
            d = np.where(d < 0, row[:, None], d)
        surface = xs - d / 2 if eye == "left" else xs + d / 2
        sx = np.clip(np.round(surface + 100).astype(int), 0, wide - 1)
        colour = np.where(is_box[..., None], crate[ys, sx], ground[ys, sx])
        if shadow is not None:
            colour = colour * shadow(eye, surface, xs, ys, d)[..., None]
        images.append(Image.fromarray((np.clip(colour, 0, 1) * 255).astype(np.uint8), "RGB"))
    return images[0], images[1]


def _blob(cx, cy, rx, ry, x, y, depth=0.5):
    inside = ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 < 1.0
    return np.where(inside, depth, 1.0).astype(np.float32)


# The shadow cases. Each returns the brightness multiplier for one eye.
def shadow_correct(eye, surface, x, y, d):
    return _blob(140, 300, 60, 30, surface, y)                 # anchored to the surface


def shadow_one_eye(eye, surface, x, y, d):
    return _blob(140, 300, 60, 30, surface, y) if eye == "left" else np.ones_like(x)


def shadow_screen_locked(eye, surface, x, y, d):
    # Same screen position in both eyes relative to infinity: no parallax at all.
    sx = x if eye == "left" else x + INFINITY
    return _blob(180, 300, 60, 30, sx, y)


def shadow_wrong_depth(eye, surface, x, y, d):
    # Parallax of a plane 9 px nearer than the ground it is drawn on.
    wrong = d + 9.0
    s = x - wrong / 2 if eye == "left" else x + wrong / 2
    return _blob(150, 300, 60, 30, s, y)


def highlight_one_eye(eye, surface, x, y, d):
    # A glint one eye catches and the other does not: brighter than the surface around it.
    return _blob(140, 300, 50, 25, surface, y, depth=1.7) if eye == "right" else np.ones_like(x)


CASES = [
    # name, shadow, expected verdict prefix, expected kind of the largest region
    ("clean", None, "PASS", None),
    ("shadow_correct", shadow_correct, "PASS", None),
    ("shadow_one_eye", shadow_one_eye, "FAIL", "ONE_EYE_ONLY"),
    ("shadow_screen_locked", shadow_screen_locked, "FAIL", "SCREEN_LOCKED"),
    ("shadow_wrong_depth", shadow_wrong_depth, "FAIL", "WRONG_DEPTH"),
    ("highlight_one_eye", highlight_one_eye, "WARN_HIGHLIGHT", None),
]
