"""tttrlib.signal / tttrlib.interpolate against scipy, bit for bit.

lfilter and FITPACK's fixed-knot curfit/splev are ports of scipy's C, and
savgol is scipy's Python over tttrlib.ndimage, so the numbers must be scipy's
exactly. scipy is the oracle here only.
"""

import numpy as np
import pytest

scipy = pytest.importorskip("scipy")
from scipy import interpolate as sci_interp  # noqa: E402
from scipy import signal as sci_signal  # noqa: E402

from tttrlib import interpolate as tt_interp  # noqa: E402
from tttrlib import signal as tt_signal  # noqa: E402

RNG = np.random.default_rng(20261006)


@pytest.mark.parametrize("trial", range(30))
def test_lfilter_iir_and_fir_bit_identical(trial):
    nb = int(RNG.integers(1, 8))
    na = int(RNG.integers(1, 8))
    b = RNG.normal(size=nb)
    a = RNG.normal(size=na)
    a[0] = RNG.uniform(0.5, 2.0) * (1 if trial % 2 else -1)
    if na > 1:
        a[1:] *= 0.2  # keep it stable enough to stay finite
    x = RNG.normal(size=int(RNG.integers(1, 400)))
    np.testing.assert_array_equal(tt_signal.lfilter(b, a, x), sci_signal.lfilter(b, a, x))


def test_lfilter_comb_and_allpass_like_the_audifier():
    x = RNG.normal(size=5000)
    d = 37
    b = np.zeros(d + 1)
    b[0] = 1.0
    a = np.zeros(d + 1)
    a[0] = 1.0
    a[d] = -0.6
    np.testing.assert_array_equal(tt_signal.lfilter(b, a, x), sci_signal.lfilter(b, a, x))


def test_lfilter_along_an_axis():
    x = RNG.normal(size=(4, 50))
    b, a = [0.3, 0.2], [1.0, -0.5]
    for axis in (0, 1, -1):
        np.testing.assert_array_equal(
            tt_signal.lfilter(b, a, x, axis=axis), sci_signal.lfilter(b, a, x, axis=axis)
        )


@pytest.mark.parametrize("window, order, deriv", [(5, 2, 0), (7, 3, 1), (11, 2, 1), (9, 4, 2),
                                                  (6, 2, 0), (21, 5, 1)])
def test_savgol_coeffs_and_filter_match_scipy(window, order, deriv):
    np.testing.assert_array_equal(
        tt_signal.savgol_coeffs(window, order, deriv=deriv, delta=0.25),
        sci_signal.savgol_coeffs(window, order, deriv=deriv, delta=0.25),
    )
    y = np.cumsum(RNG.normal(size=300))
    for mode in ("interp", "mirror", "nearest", "constant", "wrap"):
        np.testing.assert_array_equal(
            tt_signal.savgol_filter(y, window, order, deriv=deriv, delta=0.25, mode=mode),
            sci_signal.savgol_filter(y, window, order, deriv=deriv, delta=0.25, mode=mode),
            err_msg=mode,
        )


def test_savgol_along_axis_0_of_a_matrix():
    y = RNG.normal(size=(120, 3))
    np.testing.assert_array_equal(
        tt_signal.savgol_filter(y, 9, 3, deriv=1, axis=0),
        sci_signal.savgol_filter(y, 9, 3, deriv=1, axis=0),
    )


@pytest.mark.parametrize("trial", range(20))
def test_splrep_with_given_knots_and_splev_bit_identical(trial):
    m = int(RNG.integers(20, 300))
    x = np.sort(RNG.uniform(-3, 4, m))
    x = np.unique(x)
    y = np.sin(x) + 0.05 * RNG.normal(size=x.size)
    k = int(RNG.integers(1, 6))
    n_knots = int(RNG.integers(1, max(2, x.size // 8)))
    knots = np.linspace(x[1], x[-2], n_knots + 2)[1:-1]
    try:
        want = sci_interp.splrep(x, y, s=0.0, k=k, t=knots)
    except ValueError:
        with pytest.raises(ValueError):
            tt_interp.splrep(x, y, s=0.0, k=k, t=knots)
        return
    got = tt_interp.splrep(x, y, s=0.0, k=k, t=knots)
    np.testing.assert_array_equal(got[0], want[0])
    np.testing.assert_array_equal(got[1], want[1])
    assert got[2] == want[2]
    xx = np.linspace(x[0] - 0.5, x[-1] + 0.5, 777)
    for ext in (0, 1, 3):
        np.testing.assert_array_equal(
            tt_interp.splev(xx, got, ext=ext), sci_interp.splev(xx, want, ext=ext)
        )


def test_the_fcs_smoothing_call():
    # chisurf.core.fluorescence.fcs: cubic spline over interior knots on log time.
    t = np.logspace(-6, 0, 400)
    g = 1 + 1 / (1 + t / 1e-3) + 0.01 * RNG.normal(size=t.size)
    x = np.log10(t)
    knots = np.linspace(x[1], x[-1], 12 + 2)[1:-1]
    want = sci_interp.splev(x, sci_interp.splrep(x, g, s=0.0, k=3, t=knots), der=0)
    got = tt_interp.splev(x, tt_interp.splrep(x, g, s=0.0, k=3, t=knots), der=0)
    np.testing.assert_array_equal(got, want)
