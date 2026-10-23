#!/usr/bin/env python3
"""
Binocular-consistency analysis for a stereo pair.

A correct stereo render shows the SAME world to both eyes, displaced horizontally by a
disparity that depends only on depth. Everything that is attached to a surface - texture,
paint, and the shadow lying on it - must therefore move with that surface. This module
measures where that fails:

  1. SURFACE DISPARITY. A dense left->right disparity map is matched on features chosen
     to ignore illumination (locally normalised texture plus chromaticity) with a
     semi-global matcher, so a single shadow edge cannot drag the match away from the
     surface it lies on. A coarse pass finds the range to search first.
  2. RESIDUAL. The right eye is warped onto the left through that map and the two are
     compared in low-frequency log luminance. Geometry cancels. What is left is shading
     one eye sees and the other does not: binocular rivalry.
  3. CLASSIFICATION. Each residual region is re-matched on its own. A region that
     disappears when shifted is shading drawn at the wrong DEPTH (and if that shift
     lands exactly on infinity it has no parallax at all: it is locked to the screen).
     A region nothing cancels exists in ONE EYE ONLY. Each is also a SHADOW or a
     HIGHLIGHT, by whether the odd eye is darker or brighter than the surface around it.

Only numpy and Pillow are required. Disparities are reported in percent of the eye width,
so they mean the same thing at any capture resolution.

    python stereo_consistency.py pair.bmp --out report.png
    python stereo_consistency.py --watch 40 --interval 3 --out-dir incidents
    python test_stereo_consistency.py        # synthetic ground truth for every verdict
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from dataclasses import dataclass, field, asdict
from io import BytesIO
from pathlib import Path
from typing import Any, Optional

import numpy as np
from PIL import Image, ImageDraw

ANALYSIS_WIDTH = 480          # per-eye width the matcher runs at
ANALYSIS_MAX_HEIGHT = 480     # bound cost-volume storage for portrait captures too
NEAR_MARGIN_FRACTION = 0.04   # searched beyond the nearest disparity the coarse pass found
MIN_RANGE_FRACTION = 0.30     # span always searched above the farthest disparity (~0.15 m)
MAX_RANGE_FRACTION = 0.45     # ...but never more than this span (memory); ~0.1 m at 100 deg
FAR_RANGE_PX = 10             # searched below the farthest: infinity and a little beyond
SGM_P1 = 0.10                 # cost of a one-pixel disparity step between neighbours
SGM_P2 = 1.00                 # cost of a larger jump (a depth edge)
LOG_EPS = 0.03                # luminance floor: keeps black crush out of the log ratio
RESIDUAL_THRESHOLD = 0.16     # |log ratio|: one eye ~17% brighter than the other
MIN_REGION_FRACTION = 0.0004  # of the eye area; smaller blobs are aliasing, not rivalry
TOLERANCE_PX = 2              # residual is forgiven this much disparity error


@dataclass
class Region:
    kind: str                 # ONE_EYE_ONLY | WRONG_DEPTH | SCREEN_LOCKED
    box: tuple                # x0, y0, x1, y1 in left-eye fractions of width/height
    area_pct: float           # of the eye
    darker_eye: str           # which eye sees the darker (shadowed) version
    mean_ratio_pct: float     # how much darker, percent
    surface_disparity_pct: float
    shading_disparity_pct: Optional[float]  # where the shading actually sits, if it moved
    cancel_fraction: float    # share of the residual a pure shift explains
    grey: float               # 1 = pure brightness change (shadow-like), 0 = colour change
    surface_metres: Optional[float] = None  # the same two as distances, when the frusta are known
    shading_metres: Optional[float] = None
    nature: str = "shadow"    # shadow | highlight: is the odd eye darker or brighter than the
                              # surface around the region? Shadows are defects; a highlight can
                              # be honest (specular and bloom do differ between two eyes)


@dataclass
class Report:
    verdict: str
    score: float                        # area-weighted SHADOW rivalry, percent of the valid eye
    highlight_score: float              # the same for highlights, which may be legitimate
    valid_pct: float
    median_disparity_pct: float
    max_disparity_pct: float
    behind_infinity_pct: float          # share of matched pixels beyond the declared infinity
    vertical_offset_px: float
    exposure_offset_pct: float          # global brightness difference between the eyes
    far_disparity_pct: float = 0.0      # where the farthest matched content sits
    infinity_disparity_pct: Optional[float] = None  # where the declared frusta put infinity
    regions: list = field(default_factory=list)
    notes: list = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _box(a: np.ndarray, ry: int, rx: int) -> np.ndarray:
    """Mean over a (2ry+1) x (2rx+1) window, edges normalised by the true count."""
    h, w = a.shape[:2]
    pad = [(ry + 1, ry), (rx + 1, rx)] + [(0, 0)] * (a.ndim - 2)
    c = np.pad(a, pad, mode="edge").cumsum(0, dtype=np.float64).cumsum(1, dtype=np.float64)
    s = c[2 * ry + 1:, 2 * rx + 1:] - c[:h, 2 * rx + 1:] - c[2 * ry + 1:, :w] + c[:h, :w]
    return (s / ((2 * ry + 1) * (2 * rx + 1))).astype(np.float32)


def _shift_x(a: np.ndarray, d: int, fill: float) -> np.ndarray:
    """out[x] = a[x - d]; columns that fall outside take `fill`."""
    out = np.full_like(a, fill)
    w = a.shape[1]
    if d == 0:
        out[...] = a
    elif 0 < d < w:
        out[:, d:] = a[:, :w - d]
    elif -w < d < 0:
        out[:, :w + d] = a[:, -d:]
    return out


def _luma(rgb: np.ndarray) -> np.ndarray:
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def _features(rgb: np.ndarray) -> np.ndarray:
    """Illumination-tolerant matching features: normalised texture and chromaticity.

    A shadow multiplies a surface's colour by a roughly grey factor. Dividing the
    high-pass by the local contrast removes the gain from the texture, and
    chromaticity does not see a grey factor at all.
    """
    y = _luma(rgb)
    mean = _box(y, 3, 3)
    var = np.maximum(_box(y * y, 3, 3) - mean * mean, 0.0)
    texture = (y - mean) / (np.sqrt(var) + 0.02)
    total = rgb.sum(-1) + 0.06
    chroma_r = rgb[..., 0] / total
    chroma_b = rgb[..., 2] / total
    return np.stack([texture * 0.35, chroma_r * 4.0, chroma_b * 4.0], -1).astype(np.float32)


def _cost_volume(fl: np.ndarray, fr: np.ndarray, d_min: int, d_max: int) -> np.ndarray:
    big = 2.5
    volume = np.empty((d_max - d_min + 1,) + fl.shape[:2], np.float32)
    for i, d in enumerate(range(d_min, d_max + 1)):
        diff = np.abs(fl - _shift_x(fr, d, 99.0)).sum(-1)
        np.minimum(diff, big, out=diff)
        # 3x3 mean, separably and in place: this loop is the hot one.
        rows = diff.copy()
        rows[1:] += diff[:-1]
        rows[:-1] += diff[1:]
        out = volume[i]
        out[...] = rows
        out[:, 1:] += rows[:, :-1]
        out[:, :-1] += rows[:, 1:]
        out *= 1.0 / 9.0
    return volume


def _sgm_pass(volume: np.ndarray, out: np.ndarray, axis: int, reverse: bool) -> None:
    """One semi-global path: accumulate `volume` along `axis` (1 = rows, 2 = columns).

    Texture decides the disparity where there is any; this carries that decision across
    flat paint, gloves and tarmac, and makes an isolated edge (a shadow boundary) pay for
    leaving the surface it lies on.
    """
    n = volume.shape[axis]
    order = range(n - 1, -1, -1) if reverse else range(n)
    previous = None
    for i in order:
        cost = volume[:, i, :] if axis == 1 else volume[:, :, i]
        if previous is None:
            current = cost.copy()
        else:
            floor = previous.min(0, keepdims=True)
            step = np.minimum(previous, floor + SGM_P2)
            up = np.empty_like(previous)
            up[1:] = previous[:-1] + SGM_P1
            up[0] = np.inf
            down = np.empty_like(previous)
            down[:-1] = previous[1:] + SGM_P1
            down[-1] = np.inf
            np.minimum(step, up, out=step)
            np.minimum(step, down, out=step)
            current = cost + step - floor
        if axis == 1:
            out[:, i, :] += current
        else:
            out[:, :, i] += current
        previous = current


def _sgm(volume: np.ndarray) -> np.ndarray:
    out = np.zeros_like(volume)
    for axis in (2, 1):
        for reverse in (False, True):
            _sgm_pass(volume, out, axis, reverse)
    out *= 0.25
    return out


def _disparity_bounds(L: np.ndarray, R: np.ndarray) -> tuple:
    """(lowest, highest) disparity present, in analysis pixels, from a coarse full-range match.

    Asymmetric per-eye frusta put infinity at a large constant offset (the two eye windows
    are different parts of the view) and a first-person body sits a hand's width from the
    eyes, so the dense search cannot assume where disparity starts or stops. From an
    eighth of the eye width behind zero to five eighths in front is searched at low
    resolution, and the dense pass is then given the span that is occupied.
    """
    h, w = L.shape[:2]
    scale = max(1, w // 120)
    small_l = L[::scale, ::scale]
    small_r = R[::scale, ::scale]
    sw = small_l.shape[1]
    fl, fr = _features(small_l), _features(small_r)
    d_min, d_max = -sw // 8, int(sw * 0.62)
    volume = np.empty((d_max - d_min + 1,) + fl.shape[:2], np.float32)
    for i, d in enumerate(range(d_min, d_max + 1)):
        diff = np.minimum(np.abs(fl - _shift_x(fr, d, 99.0)).sum(-1), 2.5)
        volume[i] = _box(diff, 2, 2)
    volume = _sgm(volume)
    disparity, uniqueness = _wta(volume, d_min)
    # Only pixels both eyes agree on vote: each eye's exclusive periphery matches nothing
    # and would otherwise report whatever disparity its noise happened to favour.
    sh = small_l.shape[0]
    best = np.full((sh, sw), np.inf, np.float32)
    right = np.zeros((sh, sw), np.float32)
    for i, d in enumerate(range(d_min, d_max + 1)):
        cost = _shift_x(volume[i], -d, 99.0)
        better = cost < best
        best[better] = cost[better]
        right[better] = d
    columns = np.arange(sw, dtype=np.float32)[None, :].repeat(sh, 0)
    good = (uniqueness > 0.05) & (np.abs(_sample_x(right, columns - disparity) - disparity) <= 1.0)
    if good.sum() < 50:
        good = np.ones_like(good)
    low, high = np.percentile(disparity[good], [1.0, 99.5])
    return int(np.floor(low * scale)), int(np.ceil(high * scale))


def _wta(volume: np.ndarray, d_min: int):
    """Winner-take-all with a parabolic sub-pixel step and a uniqueness measure."""
    n = volume.shape[0]
    idx = volume.argmin(0)
    best = np.take_along_axis(volume, idx[None], 0)[0]
    lo = np.take_along_axis(volume, np.clip(idx - 1, 0, n - 1)[None], 0)[0]
    hi = np.take_along_axis(volume, np.clip(idx + 1, 0, n - 1)[None], 0)[0]
    denom = lo + hi - 2.0 * best
    sub = np.where(denom > 1e-6, 0.5 * (lo - hi) / np.maximum(denom, 1e-6), 0.0)
    disparity = idx + d_min + np.clip(sub, -0.5, 0.5)
    # How much worse is the best match a few disparities away? Flat regions score ~0.
    away = np.minimum(np.take_along_axis(volume, np.clip(idx - 4, 0, n - 1)[None], 0)[0],
                      np.take_along_axis(volume, np.clip(idx + 4, 0, n - 1)[None], 0)[0])
    uniqueness = (away - best) / (away + 1e-3)
    edge = (idx == 0) | (idx == n - 1)
    uniqueness[edge] = 0.0
    return disparity.astype(np.float32), uniqueness.astype(np.float32)


def _sample_x(a: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Linear sample of each row of `a` at fractional column positions `x`."""
    w = a.shape[1]
    x = np.clip(x, 0.0, w - 1.001)
    x0 = np.floor(x).astype(np.int64)
    t = (x - x0).astype(np.float32)
    if a.ndim == 3:
        g0 = np.take_along_axis(a, x0[..., None], 1)
        g1 = np.take_along_axis(a, (x0 + 1)[..., None], 1)
        return g0 * (1 - t[..., None]) + g1 * t[..., None]
    g0 = np.take_along_axis(a, x0, 1)
    g1 = np.take_along_axis(a, x0 + 1, 1)
    return g0 * (1 - t) + g1 * t


def _fill(disparity: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Normalised convolution: spread trusted disparities into untrusted pixels.

    The window is much wider than tall because a road is: on level ground disparity is
    constant along a row and changes fastest down the image.
    """
    out = disparity.copy()
    have = valid.astype(np.float32)
    for ry, rx in ((2, 12), (5, 40), (12, 120), (40, 400)):
        weight = _box(have, ry, rx)
        spread = _box(out * have, ry, rx) / np.maximum(weight, 1e-6)
        need = have < 0.5
        take = need & (weight > 0.02)
        out[take] = spread[take]
        have[take] = 1.0
        if have.min() > 0.5:
            break
    return out


def _extreme(a: np.ndarray, r: int, largest: bool) -> np.ndarray:
    """Running min or max over a (2r+1) square, separably."""
    pick = np.maximum if largest else np.minimum
    out = a.copy()
    for axis in (0, 1):
        source = out.copy()
        for k in range(1, r + 1):
            forward = np.roll(source, k, axis)
            backward = np.roll(source, -k, axis)
            if axis == 0:
                forward[:k] = source[:k]
                backward[-k:] = source[-k:]
            else:
                forward[:, :k] = source[:, :k]
                backward[:, -k:] = source[:, -k:]
            out = pick(out, pick(forward, backward))
    return out


def _dilate(mask: np.ndarray, r: int) -> np.ndarray:
    return _box(mask.astype(np.float32), r, r) > 1e-4


def _label(mask: np.ndarray):
    """8-connected components by run merging. Returns (labels, count)."""
    h, w = mask.shape
    labels = np.zeros((h, w), np.int32)
    parent = [0]

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    previous = []
    for y in range(h):
        row = mask[y]
        if not row.any():
            previous = []
            continue
        edges = np.flatnonzero(np.diff(np.concatenate(([0], row.view(np.int8), [0]))))
        runs = []
        for x0, x1 in zip(edges[::2], edges[1::2]):
            label = 0
            for px0, px1, plabel in previous:
                if px0 <= x1 and px1 >= x0 - 1:
                    root = find(plabel)
                    if label == 0:
                        label = root
                    elif root != label:
                        parent[root] = label
            if label == 0:
                label = len(parent)
                parent.append(label)
            labels[y, x0:x1] = label
            runs.append((x0, x1, label))
        previous = runs
    roots = np.array([find(i) for i in range(len(parent))], np.int32)
    labels = roots[labels]
    unique, inverse = np.unique(labels, return_inverse=True)
    return inverse.reshape(h, w).astype(np.int32), len(unique) - 1


@dataclass
class Frusta:
    """The per-eye frusta the app declared in xrEndFrame, reduced to what disparity needs."""
    infinity: float   # xL - xR of a point at infinity, as a fraction of the eye width
    focal: float      # eye widths per unit tangent
    ipd: float        # metres

    def metres(self, disparity_fraction: float) -> Optional[float]:
        excess = disparity_fraction - self.infinity
        if excess <= 1e-4:
            return None   # at or beyond infinity
        return self.ipd * self.focal / excess


def frusta_from_fov(left: dict, right: dict, ipd_metres: float = 0.064) -> Optional[Frusta]:
    """`left`/`right` hold angleLeft/angleRight in radians under the keys aL and aR."""
    try:
        ll, lr = math.tan(left["aL"]), math.tan(left["aR"])
        rl, rr = math.tan(right["aL"]), math.tan(right["aR"])
        if not all(math.isfinite(value) for value in (ll, lr, rl, rr, ipd_metres)) or ipd_metres < 0:
            return None
    except (KeyError, TypeError, ValueError):
        return None
    if lr - ll <= 1e-6 or rr - rl <= 1e-6:
        return None
    # Forward (tan = 0) lands at -tanLeft / width in each eye's image.
    return Frusta(infinity=(-ll / (lr - ll)) - (-rl / (rr - rl)), focal=1.0 / (lr - ll), ipd=ipd_metres)


def split_eyes(image: Image.Image):
    """Side-by-side preview -> (left, right). Stacked previews are not produced any more."""
    if image.width < 2 or image.width % 2 or image.height < 1:
        raise ValueError("A side-by-side image must contain two equal nonempty eyes")
    image = image.convert("RGB")
    half = image.width // 2
    return image.crop((0, 0, half, image.height)), image.crop((half, 0, half * 2, image.height))


def _prepare(eye: Image.Image, width: int) -> np.ndarray:
    height = max(16, round(eye.height * width / eye.width))
    with eye.resize((width, height), Image.BOX if eye.width >= width * 2 else Image.BILINEAR) as small:
        with small.convert("RGB") as rgb:
            return np.asarray(rgb, np.float32) / 255.0


def analyze_pair(left: Image.Image, right: Image.Image, width: int = ANALYSIS_WIDTH,
                 threshold: float = RESIDUAL_THRESHOLD, want_images: bool = True,
                 frusta: Optional[Frusta] = None):
    """Returns (Report, overlay Image or None)."""
    if not math.isfinite(threshold) or threshold <= 0:
        raise ValueError("A finite positive stereo threshold is required")
    if type(width) is not int or not 16 <= width <= ANALYSIS_WIDTH:
        raise ValueError(f"Analysis width must be in [16,{ANALYSIS_WIDTH}]")
    if min(*left.size, *right.size) <= 0:
        raise ValueError("Both eyes must have nonempty pixels")
    if left.width * right.height != right.width * left.height:
        raise ValueError("Both eyes must have the same aspect ratio")
    width = min(width, ANALYSIS_MAX_HEIGHT * left.width // left.height)
    if width < 16:
        raise ValueError("Eye aspect ratio is too narrow for bounded stereo analysis")
    L = _prepare(left, width)
    R = _prepare(right, width)
    h, w = L.shape[:2]
    notes = []

    yl, yr = _luma(L), _luma(R)
    if max(float(yl.std()), float(yr.std())) < 0.012:
        return Report("INCONCLUSIVE_NO_FEATURES", 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                      notes=["Both eyes are flat colour; nothing to match."]), None

    # ---- global checks -------------------------------------------------------------
    # Vertical offset between the eyes must be zero with parallel frusta.
    rows_l = yl[:, w // 4: 3 * w // 4].mean(1)
    rows_r = yr[:, w // 4: 3 * w // 4].mean(1)
    rows_l = rows_l - _box(rows_l[:, None], 8, 0)[:, 0]
    rows_r = rows_r - _box(rows_r[:, None], 8, 0)[:, 0]
    best_dy, best_err = 0, 1e9
    for dy in range(-8, 9):
        a = rows_l[max(0, dy): h + min(0, dy)]
        b = rows_r[max(0, -dy): h + min(0, -dy)]
        err = float(np.mean((a - b) ** 2))
        if err < best_err - 1e-9 or (abs(err - best_err) <= 1e-9 and abs(dy) < abs(best_dy)):
            best_dy, best_err = dy, err
    vertical_px = best_dy * left.height / h

    # ---- 1. surface disparity ------------------------------------------------------
    lowest, highest = _disparity_bounds(L, R)
    d_min = lowest - FAR_RANGE_PX
    if frusta is not None:
        d_min = min(d_min, int(frusta.infinity * w) - FAR_RANGE_PX // 2)
    # The coarse pass under-reports the near end (a lap or a glove is flat colour and a
    # few coarse pixels wide), so the span never drops below the floor; it only grows.
    d_max = max(highest + int(round(w * NEAR_MARGIN_FRACTION)), d_min + int(round(w * MIN_RANGE_FRACTION)))
    d_max = min(d_max, d_min + int(round(w * MAX_RANGE_FRACTION)))
    fl, fr = _features(L), _features(R)
    volume = _sgm(_cost_volume(fl, fr, d_min, d_max))
    disp_l, unique_l = _wta(volume, d_min)

    # Right-referenced disparity from the same volume: cost_r[d](x) = cost_l[d](x + d).
    # A running minimum, so the second volume never has to exist.
    best_r = np.full((h, w), np.inf, np.float32)
    disp_r = np.zeros((h, w), np.float32)
    for i, d in enumerate(range(d_min, d_max + 1)):
        cost = _shift_x(volume[i], -d, 99.0)
        better = cost < best_r
        best_r[better] = cost[better]
        disp_r[better] = d
    del best_r

    xs = np.arange(w, dtype=np.float32)[None, :].repeat(h, 0)
    back = _sample_x(disp_r, xs - disp_l)
    consistent = np.abs(back - disp_l) <= 1.5
    in_frame = (xs - disp_l >= 1.0) & (xs - disp_l <= w - 2.0)
    textured = unique_l > 0.04
    trusted = consistent & in_frame & textured

    if trusted.mean() < 0.03:
        return Report("INCONCLUSIVE_NO_MATCH", 0.0, 0.0, round(100 * float(trusted.mean()), 2), 0.0, 0.0, 0.0,
                      round(vertical_px, 2), 0.0,
                      notes=["Under 3% of the image could be matched between the eyes. Either the view is "
                             "featureless or the two eyes do not show the same scene."]), None

    surface = _fill(disp_l, trusted)
    # Occlusions are legitimate differences (one eye sees round a corner) and are
    # excluded. They are found GEOMETRICALLY, from the surface itself: a left pixel is
    # hidden in the right eye when a nearer surface lands on the same right-eye column.
    # "The other eye does not match here" cannot be the test - that is also exactly what
    # a shadow present in one eye looks like, and it would excuse the very thing this
    # is looking for.
    target = np.clip(np.round(xs - surface).astype(np.int64), 0, w - 1)
    flat = (np.arange(h, dtype=np.int64)[:, None] * w + target).ravel()
    nearest = np.full(h * w, -np.inf, np.float32)
    np.maximum.at(nearest, flat, surface.ravel())
    occluded = surface < nearest[flat].reshape(h, w) - 2.5
    # Despeckle before growing: a noisy disparity on a flat-coloured near surface
    # "occludes" single pixels all over it, and growing each of those would excuse the
    # whole surface.
    occluded &= _box(occluded.astype(np.float32), 2, 2) > 0.35
    excluded = _dilate(occluded, 1) | ~((xs - surface >= 1.0) & (xs - surface <= w - 2.0))
    # The left eye's periphery has no counterpart at all, and how wide that strip is
    # depends on depth: a held item or an arm leaves the right eye's frame long before
    # the road behind it does. A pixel that found no consistent match, and sits where
    # the nearest surface on its rows would already be out of frame, is not compared.
    row_near = np.where(trusted, disp_l, -np.inf).max(1)
    row_near = np.where(np.isfinite(row_near), row_near, float(np.median(disp_l[trusted])))
    row_near = _extreme(row_near[:, None], 12, True)[:, 0]
    excluded |= ~trusted & (xs < row_near[:, None] + 2.0)
    # ---- 2. residual ----------------------------------------------------------------
    # Next to a depth edge a pixel may belong to either side, and the filled surface is
    # least certain exactly there. So each pixel is compared through the nearest and the
    # farthest disparity in its neighbourhood as well as its own, and keeps whichever
    # agrees best. Inside a surface the three coincide and nothing is forgiven.
    warped = _sample_x(R, xs - surface)
    log_l = np.log(_box(yl, 2, 2) + LOG_EPS)
    smooth_r = _box(yr, 2, 2)
    residual = None
    exposure = 0.0
    for index, candidate in enumerate((surface, _extreme(surface, 4, True), _extreme(surface, 4, False))):
        log_w = np.log(_sample_x(smooth_r, xs - candidate) + LOG_EPS)
        if index == 0:
            exposure = float(np.median((log_l - log_w)[~excluded])) if (~excluded).any() else 0.0
        for dx in range(-TOLERANCE_PX, TOLERANCE_PX + 1):
            for dy in (-1, 0, 1):
                shifted = np.roll(_shift_x(log_w, dx, 0.0), dy, 0)
                e = log_l - shifted - exposure
                residual = e if residual is None else np.where(np.abs(e) < np.abs(residual), e, residual)
    border = 6
    excluded[:border] = True
    excluded[-border:] = True
    excluded[:, :border] = True
    excluded[:, -border:] = True
    valid = ~excluded
    residual = np.where(valid, residual, 0.0)

    mask = np.abs(residual) > threshold
    # A rivalry region is an area, not an edge: require support in a 5x5 neighbourhood.
    mask &= _box(mask.astype(np.float32), 2, 2) > 0.45
    labels, count = _label(mask)

    # ---- 3. classify ----------------------------------------------------------------
    matched = disp_l[trusted]
    far = float(np.percentile(matched, 2))
    # Where zero parallax lands: the declared frusta say exactly; without them the
    # farthest content in view is the best available stand-in.
    infinity_px = frusta.infinity * w if frusta is not None else far
    regions = []
    eye_area = float(h * w)
    chroma_l = L / (L.sum(-1, keepdims=True) + 0.06)
    chroma_w = warped / (warped.sum(-1, keepdims=True) + 0.06)
    blur_l = _box(yl, 2, 2)
    blur_r = _box(yr, 2, 2)
    for label in range(1, count + 1):
        ys, xs_r = np.nonzero(labels == label)
        area = len(ys)
        if area < MIN_REGION_FRACTION * eye_area:
            labels[ys, xs_r] = 0
            continue
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs_r.min(), xs_r.max() + 1
        mean_residual = float(residual[ys, xs_r].mean())
        surf = float(np.median(surface[ys, xs_r]))
        grey = 1.0 - min(1.0, float(np.abs(chroma_l[ys, xs_r] - chroma_w[ys, xs_r]).sum(-1).mean()) / 0.12)

        # Re-match the region's own low-frequency luminance over every shift. If the
        # shading is a copy of the other eye's at a different disparity, some shift
        # cancels it.
        my0, my1 = max(0, y0 - 3), min(h, y1 + 3)
        mx0, mx1 = max(0, x0 - 6), min(w, x1 + 6)
        patch = np.log(blur_l[my0:my1, mx0:mx1] + LOG_EPS)
        base = float(np.mean((patch - np.log(_box(_luma(warped), 2, 2)[my0:my1, mx0:mx1] + LOG_EPS) - exposure) ** 2))
        best_shift, best_mse = None, base
        for d in range(d_min, d_max + 1):
            if mx0 - d < 0 or mx1 - d > w:
                continue
            other = np.log(blur_r[my0:my1, mx0 - d:mx1 - d] + LOG_EPS)
            mse = float(np.mean((patch - other - exposure) ** 2))
            if mse < best_mse:
                best_mse, best_shift = mse, d
        cancel = 0.0 if base <= 1e-9 else 1.0 - best_mse / base
        shading_disparity = None
        kind = "ONE_EYE_ONLY"
        if best_shift is not None and cancel > 0.75 and abs(best_shift - surf) > TOLERANCE_PX + 0.5:
            shading_disparity = best_shift
            # No parallax at all puts the shading exactly where infinity is: it was drawn
            # at the same place on both screens instead of on the surface.
            locked = abs(best_shift - infinity_px) <= 1.5 and surf - infinity_px > 4
            kind = "SCREEN_LOCKED" if locked else "WRONG_DEPTH"

        # Shadow or highlight? Compare each eye's region with the agreed surface around
        # it. The eye that departs from its surroundings is the odd one, and the
        # direction it departs in says which this is.
        ry0, ry1, rx0, rx1 = max(0, y0 - 8), min(h, y1 + 8), max(0, x0 - 8), min(w, x1 + 8)
        inside = labels[ry0:ry1, rx0:rx1] == label
        ring = _dilate(inside, 6) & ~_dilate(inside, 1) & valid[ry0:ry1, rx0:rx1] & ~mask[ry0:ry1, rx0:rx1]
        nature = "shadow"
        if ring.sum() >= 12:
            around = float(np.median(log_l[ry0:ry1, rx0:rx1][ring]))
            in_left = float(log_l[ys, xs_r].mean())
            in_right = in_left - mean_residual
            odd = in_left if abs(in_left - around) >= abs(in_right - around) else in_right
            nature = "highlight" if odd > around else "shadow"

        regions.append(Region(
            nature=nature,
            kind=kind,
            box=(round(x0 / w, 4), round(y0 / h, 4), round(x1 / w, 4), round(y1 / h, 4)),
            area_pct=round(100.0 * area / eye_area, 3),
            darker_eye="left" if mean_residual < 0 else "right",
            mean_ratio_pct=round(100.0 * (1.0 - float(np.exp(-abs(mean_residual)))), 1),
            surface_disparity_pct=round(100.0 * surf / w, 2),
            shading_disparity_pct=None if shading_disparity is None else round(100.0 * shading_disparity / w, 2),
            cancel_fraction=round(max(0.0, cancel), 2),
            grey=round(grey, 2),
            surface_metres=_metres(frusta, surf / w),
            shading_metres=None if shading_disparity is None else _metres(frusta, shading_disparity / w),
        ))
    regions.sort(key=lambda r: -r.area_pct * r.mean_ratio_pct)

    valid_fraction = float(valid.mean())
    flagged = labels > 0
    weight = {"shadow": 0.0, "highlight": 0.0}
    for region in regions:
        weight[region.nature] += region.area_pct * (-np.log(1.0 - region.mean_ratio_pct / 100.0)) / threshold
    score = weight["shadow"] / max(valid_fraction, 1e-3)
    highlight_score = weight["highlight"] / max(valid_fraction, 1e-3)
    shadows = [r for r in regions if r.nature == "shadow"]
    behind = float((matched < infinity_px - 2.0).mean()) if frusta is not None else 0.0
    if frusta is not None and far < infinity_px - 0.02 * w:
        notes.append(f"The farthest content sits at {100 * far / w:.1f}% disparity but the declared frusta put "
                     f"infinity at {100 * frusta.infinity:.1f}%. Either the image does not cover the frustum the "
                     "app declared (wrong crop or projection offset), or this capture is not the whole eye "
                     "(a zoomed or panned preview).")

    if behind > 0.05:
        notes.append("More than 5% of matched pixels sit BEHIND the declared infinity: the eyes would have to "
                     "diverge to fuse them. Check eye order, convergence offset and the sign of the separation.")
    if frusta is None and far < -2.0:
        notes.append("Disparity is negative over part of the image. With symmetric frusta that is behind "
                     "infinity (swapped eyes or an inverted separation); with asymmetric ones it is normal - "
                     "pass the frusta to tell which.")
    if abs(vertical_px) >= 2:
        notes.append(f"The eyes are offset vertically by ~{vertical_px:.0f} px; parallel frusta should give zero.")
    if abs(exposure) > 0.05:
        notes.append(f"Global brightness differs by {100 * (np.exp(abs(exposure)) - 1):.0f}% between the eyes "
                     "(per-eye exposure, tone mapping or bloom).")
    if float(np.percentile(matched, 98)) - far < 1.0 and (frusta is None or abs(far - infinity_px) < 1.5):
        notes.append("Every matched pixel has the same disparity: both eyes show the same image (no stereo).")

    if valid_fraction < 0.2:
        verdict = "INCONCLUSIVE_LOW_COVERAGE"
        notes.append(f"Only {100 * valid_fraction:.0f}% of the eye could be compared (occlusion, a surface closer "
                     "than the search range, or an image that is mostly one eye's exclusive periphery).")
    elif (score >= 1.0 or any(r.area_pct >= 0.5 for r in shadows) or
          any(r.kind != "ONE_EYE_ONLY" and r.area_pct >= 0.1 for r in shadows)):
        # A region a pure shift cancels is unambiguous even when small: it is the two
        # crescents a displaced shadow leaves on either side of where it should be.
        verdict = "FAIL_RIVALRY"
    elif highlight_score >= 1.0 or any(r.area_pct >= 0.5 for r in regions):
        # One eye is BRIGHTER than the surface around it. Specular, bloom and emissive
        # effects do that honestly, and also when they are drawn for one eye only.
        verdict = "WARN_HIGHLIGHT_RIVALRY"
    elif regions:
        verdict = "WARN_MINOR_RIVALRY"
    else:
        verdict = "PASS"

    report = Report(
        verdict=verdict,
        score=round(score, 3),
        highlight_score=round(highlight_score, 3),
        valid_pct=round(100.0 * valid_fraction, 1),
        median_disparity_pct=round(100.0 * float(np.median(matched)) / w, 2),
        max_disparity_pct=round(100.0 * float(np.percentile(matched, 99)) / w, 2),
        behind_infinity_pct=round(100.0 * behind, 2),
        vertical_offset_px=round(vertical_px, 2),
        exposure_offset_pct=round(100.0 * (float(np.exp(abs(exposure))) - 1.0), 1),
        far_disparity_pct=round(100.0 * far / w, 2),
        infinity_disparity_pct=None if frusta is None else round(100.0 * frusta.infinity, 2),
        regions=regions[:12],
        notes=notes,
    )
    overlay = _overlay(L, R, warped, residual, valid, labels > 0, surface, regions, threshold, d_max) if want_images else None
    return report, overlay


def _metres(frusta: Optional[Frusta], disparity_fraction: float) -> Optional[float]:
    if frusta is None:
        return None
    metres = frusta.metres(disparity_fraction)
    return None if metres is None else round(metres, 2)


_KIND_COLOUR = {"ONE_EYE_ONLY": (255, 60, 60), "WRONG_DEPTH": (255, 190, 40), "SCREEN_LOCKED": (255, 80, 255)}


def _overlay(L, R, warped, residual, valid, flagged, surface, regions, threshold, d_max) -> Image.Image:
    """2x2 sheet: left eye and right eye with the regions boxed, the residual, the disparity."""
    h, w = L.shape[:2]

    def to_image(a):
        return Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8), "RGB")

    # Residual: grey where the eyes agree, red where the LEFT eye is darker, blue where
    # the RIGHT eye is darker, dimmed where the comparison was excluded.
    grey = _luma(L)[..., None].repeat(3, -1) * 0.45
    strength = np.clip(np.abs(residual) / (threshold * 2.5), 0, 1)[..., None]
    colour = np.where((residual < 0)[..., None], np.array([1.0, 0.15, 0.1], np.float32),
                      np.array([0.1, 0.45, 1.0], np.float32))
    heat = grey * (1 - strength) + colour * strength
    heat = np.where(valid[..., None], heat, grey * 0.35 + np.array([0.0, 0.12, 0.0], np.float32))

    d = np.clip(surface / max(1.0, d_max), 0, 1)
    disparity = np.stack([d, np.sqrt(d) * 0.8, 1.0 - d], -1) * np.where(valid, 1.0, 0.45)[..., None]

    left_image, right_image = to_image(L), to_image(R)
    heat_image, disparity_image = to_image(heat), to_image(disparity)
    draw_l, draw_r, draw_h = ImageDraw.Draw(left_image), ImageDraw.Draw(right_image), ImageDraw.Draw(heat_image)
    for index, region in enumerate(regions[:12]):
        x0, y0, x1, y1 = region.box
        colour = _KIND_COLOUR[region.kind] if region.nature == "shadow" else (120, 210, 255)
        box = (x0 * w - 3, y0 * h - 3, x1 * w + 3, y1 * h + 3)
        shift = region.surface_disparity_pct / 100.0 * w
        for draw, dx in ((draw_l, 0.0), (draw_h, 0.0), (draw_r, -shift)):
            draw.rectangle((box[0] + dx, box[1], box[2] + dx, box[3]), outline=colour, width=2)
            draw.text((box[0] + dx + 2, box[1] - 11), str(index + 1), fill=colour)

    sheet = Image.new("RGB", (w * 2 + 6, h * 2 + 6 + 28), (12, 14, 18))
    sheet.paste(left_image, (0, 14))
    sheet.paste(right_image, (w + 6, 14))
    sheet.paste(heat_image, (0, h + 6 + 28))
    sheet.paste(disparity_image, (w + 6, h + 6 + 28))
    draw = ImageDraw.Draw(sheet)
    draw.text((4, 1), "LEFT eye", fill=(230, 230, 230))
    draw.text((w + 10, 1), "RIGHT eye (boxes moved by the surface disparity)", fill=(230, 230, 230))
    draw.text((4, h + 6 + 15), "RESIDUAL: red = left eye darker, blue = right darker, green = not compared. Pale blue boxes = highlights", fill=(230, 230, 230))
    draw.text((w + 10, h + 6 + 15), "SURFACE DISPARITY: blue = far, yellow = near", fill=(230, 230, 230))
    return sheet


def analyze_image(image: Image.Image, **kwargs):
    left, right = split_eyes(image)
    with left, right:
        return analyze_pair(left, right, **kwargs)


def analyze_bytes(data: bytes, **kwargs):
    with Image.open(BytesIO(data)) as image:
        return analyze_image(image, **kwargs)


def describe(report: Report) -> str:
    lines = [f"{report.verdict}  shadow-score={report.score}  highlight-score={report.highlight_score}  valid={report.valid_pct}%  "
             f"disparity median={report.median_disparity_pct}% p99={report.max_disparity_pct}% of eye width  "
             f"behind-infinity={report.behind_infinity_pct}%  vertical={report.vertical_offset_px}px  "
             f"exposure-diff={report.exposure_offset_pct}%"
             + ("" if report.infinity_disparity_pct is None else
                f"  far={report.far_disparity_pct}% infinity={report.infinity_disparity_pct}%")]
    for index, r in enumerate(report.regions):
        where = f"x {r.box[0]:.2f}-{r.box[2]:.2f} y {r.box[1]:.2f}-{r.box[3]:.2f}"
        extra = ""
        if r.shading_disparity_pct is not None:
            extra = f", shading sits at {r.shading_disparity_pct}% vs surface {r.surface_disparity_pct}%"
            if r.surface_metres is not None:
                shading = "infinity" if r.shading_metres is None else f"{r.shading_metres} m"
                extra += f" = {shading} vs {r.surface_metres} m"
        lines.append(f"  {index + 1}. {r.nature.upper()} {r.kind} {where} area={r.area_pct}% {r.darker_eye} eye darker by "
                     f"{r.mean_ratio_pct}% (grey={r.grey}, shift explains {int(r.cancel_fraction * 100)}%{extra})")
    lines.extend("  note: " + n for n in report.notes)
    return "\n".join(lines)


# ---- capture from a running simulator -------------------------------------------------

def watch(count: int, interval: float, out_dir: Path, directory: Optional[Path] = None,
          keep_all: bool = False, threshold: float = RESIDUAL_THRESHOLD) -> list:
    """Sample retained projection frames with the geometry belonging to each image."""
    from artifact_workflows import CaptureWorkflow, captured_frusta
    from runtime_artifacts import RuntimeArtifacts, simulator_directory

    if type(count) is not int or count <= 0:
        raise ValueError("Watch count must be a positive integer")
    if not math.isfinite(interval) or interval < 0:
        raise ValueError("Watch interval must be finite and nonnegative")
    if not math.isfinite(threshold) or threshold <= 0:
        raise ValueError("A finite positive stereo threshold is required")
    out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    log = out_dir / "stereo_watch.jsonl"
    artifacts = RuntimeArtifacts(directory if directory is not None else simulator_directory())
    workflow = CaptureWorkflow(artifacts)
    try:
        for index in range(count):
            started = time.monotonic()
            entry = {"index": index, "time": time.strftime("%H:%M:%S")}
            sheet = None
            try:
                data, metadata = workflow.capture(source="stereo_consistency")
                if data is None:
                    raise TimeoutError("No projection screenshot arrived")
                frusta = captured_frusta(metadata, frusta_from_fov)
                report, sheet = analyze_bytes(data, threshold=threshold, frusta=frusta)
                entry.update(report.to_dict(), capture=metadata,
                             frusta=asdict(frusta) if frusta is not None else None)
                print(f"[{index:03d}] {describe(report)}", flush=True)
            except Exception as error:
                entry.update(verdict="CAPTURE_FAILED", error=str(error))
                print(f"[{index:03d}] capture failed: {error}", flush=True)
            results.append(entry)
            with log.open("a", encoding="utf-8") as output:
                output.write(json.dumps(entry, allow_nan=False) + "\n")
            if sheet is not None:
                with sheet:
                    if keep_all or entry["verdict"].startswith(("FAIL", "WARN")):
                        sheet.save(out_dir / f"frame{index:03d}_{entry['verdict']}.png")
                        with Image.open(BytesIO(data)) as pair:
                            pair.save(out_dir / f"frame{index:03d}_pair.png")
            if index + 1 < count:
                time.sleep(max(0.0, interval - (time.monotonic() - started)))
    finally:
        workflow.close()
        try:
            workflow.disconnect()
        finally:
            artifacts.close()
    return results


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", nargs="?", help="side-by-side stereo image (left eye on the left)")
    parser.add_argument("--right", help="right-eye image, when `image` is the left eye alone")
    parser.add_argument("--out", help="write the report sheet here")
    parser.add_argument("--json", action="store_true", help="print the report as JSON")
    parser.add_argument("--threshold", type=float, default=RESIDUAL_THRESHOLD)
    parser.add_argument("--watch", type=int, metavar="N", help="sample a running simulator N times")
    parser.add_argument("--interval", type=float, default=2.0)
    parser.add_argument("--out-dir", default="stereo_watch")
    parser.add_argument("--keep-all", action="store_true")
    parser.add_argument("--sim-dir", help="simulator data directory (default SIMXR_DATA_DIR or platform data directory)")
    args = parser.parse_args(argv)

    if not math.isfinite(args.threshold) or args.threshold <= 0:
        parser.error("--threshold must be finite and positive")
    if args.watch is not None:
        if args.watch <= 0 or not math.isfinite(args.interval) or args.interval < 0:
            parser.error("--watch must be positive and --interval finite and nonnegative")
        results = watch(args.watch, args.interval, Path(args.out_dir),
                        Path(args.sim_dir) if args.sim_dir else None, args.keep_all, args.threshold)
        failing = sum(1 for r in results if r["verdict"].startswith("FAIL"))
        missing = sum(1 for r in results if r["verdict"] == "CAPTURE_FAILED")
        print(f"{len(results) - missing} frames, {failing} failing, {missing} captures failed")
        return 1 if failing or missing or not results else 0

    if not args.image:
        parser.error("give an image, or --watch N")
    if args.right:
        with Image.open(args.image) as left, Image.open(args.right) as right:
            report, sheet = analyze_pair(left, right, threshold=args.threshold)
    else:
        with Image.open(args.image) as image:
            report, sheet = analyze_image(image, threshold=args.threshold)
    print(json.dumps(report.to_dict(), indent=2) if args.json else describe(report))
    if args.out and sheet is not None:
        sheet.save(args.out)
    return 1 if report.verdict.startswith("FAIL") else 0


if __name__ == "__main__":
    sys.exit(main())
