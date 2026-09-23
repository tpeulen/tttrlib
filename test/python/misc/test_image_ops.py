# SPDX-License-Identifier: BSD-3-Clause
"""Binding-level parity for the Simd-concept image kernels (ImageOps.i).

Every kernel's arithmetic is already pinned against brute-force references in
test/cpp/; this file pins the *seam*: numpy in, numpy out, dtypes, shapes and
error paths through tttrlib.median_filter / resize / estimate_drift / sobel /
laplace / gaussian_blur / warp_affine / histogram / moments.
"""

import numpy as np
import pytest

import tttrlib


@pytest.fixture
def rng():
    return np.random.RandomState(42)


def _clamped_window(a, r, c, shape):
    """Reference window gather with replicated edges."""
    rows, cols = a.shape
    offs = {
        "rhomb3x3": [(-1, 0), (0, -1), (0, 0), (0, 1), (1, 0)],
        "square3x3": [(i, j) for i in (-1, 0, 1) for j in (-1, 0, 1)],
        "square5x5": [(i, j) for i in (-2, 1 + 2, 2) for j in (-2, -1, 0, 1, 2)],
    }
    offs["square5x5"] = [(i, j) for i in range(-2, 3) for j in range(-2, 3)]
    out = []
    for dr, dc in offs[shape]:
        rr = min(max(r + dr, 0), rows - 1)
        cc = min(max(c + dc, 0), cols - 1)
        out.append(a[rr, cc])
    return np.asarray(out)


@pytest.mark.parametrize("shape", ["rhomb3x3", "square3x3", "square5x5"])
def test_median_matches_reference(rng, shape):
    a = (rng.rand(21, 17) * 100).astype(np.uint16)
    got = tttrlib.median_filter(a, shape)
    ref = np.empty_like(a)
    for r in range(a.shape[0]):
        for c in range(a.shape[1]):
            ref[r, c] = np.median(_clamped_window(a, r, c, shape))
    assert got.dtype == np.uint16
    np.testing.assert_array_equal(got, ref)


def test_min_max_midpoint(rng):
    a = rng.rand(15, 13)
    lo = tttrlib.min_filter(a, "square3x3")
    hi = tttrlib.max_filter(a, "square3x3")
    mid = tttrlib.midpoint_filter(a, "square3x3")
    ref_lo = np.empty_like(a)
    ref_hi = np.empty_like(a)
    for r in range(a.shape[0]):
        for c in range(a.shape[1]):
            w = _clamped_window(a, r, c, "square3x3")
            ref_lo[r, c] = w.min()
            ref_hi[r, c] = w.max()
    np.testing.assert_allclose(lo, ref_lo, atol=1e-15)
    np.testing.assert_allclose(hi, ref_hi, atol=1e-15)
    np.testing.assert_allclose(mid, (ref_lo + ref_hi) / 2, atol=1e-15)


def test_rank_filter_rejects_unknown_shape(rng):
    a = rng.rand(5, 5)
    with pytest.raises(ValueError):
        tttrlib.median_filter(a, "circle7x7")
    with pytest.raises(TypeError):
        tttrlib.median_filter(a.astype(np.int32), "square3x3")


def test_resize_area_exact_integer(rng):
    a = rng.rand(32, 24)
    got = tttrlib.resize(a, 16, 12, "area")
    # exact 2x2 mean
    blocks = a.reshape(16, 2, 12, 2)
    np.testing.assert_allclose(got, blocks.mean(axis=(1, 3)), atol=1e-12)
    up = tttrlib.resize(a, 64, 48, "area")
    np.testing.assert_allclose(up.reshape(32, 2, 24, 2).mean(axis=(1, 3)), a, atol=1e-12)


def test_resize_bilinear_matches_manual(rng):
    a = rng.rand(9, 7)
    dr, dc = 19, 13
    got = tttrlib.resize(a, dr, dc, "bilinear")
    for r in range(dr):
        sy = (r + 0.5) * 9 / dr - 0.5
        y0 = min(max(int(np.floor(sy)), 0), 8)
        y1 = min(y0 + 1, 8)
        fy = min(max(sy - y0, 0), 1)
        for c in range(dc):
            sx = (c + 0.5) * 7 / dc - 0.5
            x0 = min(max(int(np.floor(sx)), 0), 6)
            x1 = min(x0 + 1, 6)
            fx = min(max(sx - x0, 0), 1)
            top = a[y0, x0] * (1 - fx) + a[y0, x1] * fx
            bot = a[y1, x0] * (1 - fx) + a[y1, x1] * fx
            assert abs(got[r, c] - (top * (1 - fy) + bot * fy)) < 1e-12


def test_bicubic_reproduces_constant_and_is_sharper_than_bilinear(rng):
    konst = np.full((16, 16), 3.25)
    np.testing.assert_allclose(tttrlib.resize(konst, 33, 33, "bicubic"), 3.25, atol=1e-12)
    # a sharp edge: bicubic overshoots (negative lobes), bilinear cannot
    edge = np.zeros((64, 64))
    edge[:, 32:] = 1.0
    up_b = tttrlib.resize(edge, 128, 128, "bilinear")
    up_c = tttrlib.resize(edge, 128, 128, "bicubic")
    assert up_b.min() >= 0.0
    assert up_c.min() < 0.0


def test_gaussian_blur_approximates_scipy(rng):
    scipy = pytest.importorskip("scipy.ndimage")
    # band-limited input: on white noise the two kernels' L2 norms (not their
    # shapes) differ by ~20% and the comparison is meaningless -- same lesson
    # as the C++ A/B
    noise = rng.rand(40, 32)
    a = scipy.gaussian_filter(noise, 1.0, mode="nearest")
    got = tttrlib.gaussian_blur(a, 2.0)
    ref = scipy.gaussian_filter(a, 2.0, mode="nearest")
    # 3-box is an approximation: 5% of the value range on band-limited input
    assert np.abs(got - ref).max() < 0.05 * ref.max()


def test_drift_recovers_synthetic_shift(rng):
    noise = rng.rand(120, 100)
    ref = np.ascontiguousarray(tttrlib.gaussian_blur(noise, 2.5))
    img = np.ascontiguousarray(np.roll(np.roll(ref, 4, axis=1), -3, axis=0))
    dx, dy, score = tttrlib.estimate_drift(ref, img, 8)
    assert abs(dx - 4.0) < 0.3
    assert abs(dy + 3.0) < 0.3
    # a circular roll matches exactly inside the margin, so 0 is legitimate
    assert score >= 0.0
    flat = np.zeros((64, 64))
    dx0, dy0, s0 = tttrlib.estimate_drift(flat, flat, 4)
    assert (dx0, dy0, s0) == (0.0, 0.0, 0.0)


def test_gradients_match_numpy_convolution(rng):
    a = rng.rand(19, 23)
    dx = tttrlib.sobel_dx(a)
    dy = tttrlib.sobel_dy(a)
    lp = tttrlib.laplace(a)
    # replicate-pad then convolve with the same kernels
    p = np.pad(a, 1, mode="edge")
    kx = np.array([[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]], float)
    ky = kx.T
    kl = np.array([[-1, -1, -1], [-1, 8, -1], [-1, -1, -1]], float)
    from numpy.lib.stride_tricks import sliding_window_view

    win = sliding_window_view(p, (3, 3))
    np.testing.assert_allclose(dx, (win * kx).sum(axis=(-2, -1)), atol=1e-12)
    np.testing.assert_allclose(dy, (win * ky).sum(axis=(-2, -1)), atol=1e-12)
    np.testing.assert_allclose(lp, (win * kl).sum(axis=(-2, -1)), atol=1e-12)
    np.testing.assert_array_equal(tttrlib.sobel_dx(np.full((5, 5), 2.0)), 0.0)


def test_warp_affine_identity_and_translation(rng):
    a = rng.rand(21, 19)
    ident = tttrlib.warp_affine(a, np.eye(2, 3).ravel())
    np.testing.assert_allclose(ident, a, atol=1e-15)
    # forward translation by (+2, -1) means dst(x, y) = src(x - 2, y + 1)
    t = np.array([1.0, 0, 2, 0, 1, -1])
    got = tttrlib.warp_affine(a, t)
    assert got.shape == a.shape
    # interior samples must equal the shifted source exactly (integer shift):
    # dst(r, c) = a(r + 1, c - 2), so compare from column 2 on
    np.testing.assert_allclose(got[:19, 2:19], a[1:20, :17], atol=1e-15)
    with pytest.raises(RuntimeError):
        tttrlib.warp_affine(a, np.array([1.0, 2, 0, 2, 4, 0]))


def test_histogram_and_moments(rng):
    a = (rng.rand(31, 29) * 100).astype(np.uint16)
    hist = tttrlib.image_histogram(a, 0, 99)
    assert hist.sum() == a.size
    np.testing.assert_allclose(hist, np.bincount(a.ravel(), minlength=100), atol=0)
    mask = np.zeros_like(a, dtype=np.uint8)
    mask[::2] = 1
    hist_m = tttrlib.image_histogram(a, 0, 99, mask)
    assert hist_m.sum() == mask.sum()

    af = rng.rand(31, 29)
    m = tttrlib.moments(af)
    m00, m10, m01, m11, m20, m02 = m
    assert m00 == pytest.approx(af.sum(), abs=1e-9)
    assert m10 == pytest.approx((af.sum(axis=0) * np.arange(29)).sum(), abs=1e-9)
    assert m01 == pytest.approx((af.sum(axis=1) * np.arange(31)).sum(), abs=1e-9)
    assert m11 == pytest.approx(
        np.outer(np.arange(31), np.arange(29)).ravel() @ af.ravel(), abs=1e-7
    )
    # Gaussian spot centroid
    yy, xx = np.mgrid[0:31, 0:29]
    spot = np.exp(-((xx - 14.3) ** 2 + (yy - 17.6) ** 2) / (2 * 2.5**2))
    sm = tttrlib.moments(spot)
    assert sm[1] / sm[0] == pytest.approx(14.3, abs=0.01)
    assert sm[2] / sm[0] == pytest.approx(17.6, abs=0.01)


def test_integral_and_rect_sum(rng):
    a = (rng.rand(17, 19) * 1000).astype(np.uint16)
    table = tttrlib.integral_image_u16(a)
    assert table.shape == (18 * 20,)
    t = table.reshape(18, 20)
    np.testing.assert_array_equal(t[1:, 1:], a.cumsum(axis=0).cumsum(axis=1))
    s = tttrlib.rect_sum_u64(table, 19, 3, 5, 9, 12)
    assert s == pytest.approx(a[3:10, 5:13].sum(), abs=0)
