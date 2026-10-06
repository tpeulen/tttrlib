"""``tttrlib.signal`` -- the part of scipy.signal ChiSurf uses, without scipy.

``lfilter`` is scipy's direct-form-II-transposed loop (``_lfilter.cc``) in
C++ (``Signal.h``); ``savgol_coeffs``/``savgol_filter`` are scipy's own
Python, run over ``tttrlib.ndimage.convolve1d`` (itself a bit-identical port
of scipy.ndimage) and numpy's least squares. Same names, arguments and
numbers as scipy 1.18 for the cases covered (1-D float input; 2-D input
filtered along ``axis``).
"""

import math

import numpy as np

import tttrlib as _t
from tttrlib import ndimage as _ndi

__all__ = ["lfilter", "savgol_coeffs", "savgol_filter"]


def lfilter(b, a, x, axis=-1):
    """``scipy.signal.lfilter(b, a, x, axis)`` with initial rest (no ``zi``)."""
    b = np.atleast_1d(np.asarray(b, dtype=np.float64))
    a = np.atleast_1d(np.asarray(a, dtype=np.float64))
    if b.ndim != 1 or b.size == 0:
        raise ValueError("Parameter b is not a non-empty 1d array")
    if a.ndim != 1 or a.size == 0:
        raise ValueError("Parameter a is not a non-empty 1d array")
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 0:
        raise ValueError("x must be at least 1-D")
    moved = np.moveaxis(x, axis, -1)
    lines = np.ascontiguousarray(moved).reshape(-1, moved.shape[-1])
    out = np.empty_like(lines)
    for i, line in enumerate(lines):
        out[i] = _t.signal_lfilter(b, a, line)
    return np.moveaxis(out.reshape(moved.shape), -1, axis)


def _lstsq(a, b, rcond=None):
    # scipy.signal._polyutils._lstsq on numpy input: scipy.linalg.lstsq(a, b,
    # cond=eps), LAPACK gelsd -- numpy.linalg.lstsq runs the same driver.
    if rcond is None:
        rcond = np.finfo(a.dtype).eps
    return np.linalg.lstsq(a, b, rcond=rcond)


def _polyfit(x, y, deg):
    # scipy.signal._polyutils.polyfit (full=False, w=None, cov=False). Not
    # numpy.polyfit: the Vandermonde matrix is x**powers, not np.vander's
    # cumulative products, and the two differ in the last bit.
    order = int(deg) + 1
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    rcond = x.shape[0] * np.finfo(x.dtype).eps
    powers = np.flip(np.arange(order, dtype=x.dtype))
    lhs = x[:, None] ** powers[None, :]
    scale = np.sqrt(np.sum(lhs * lhs, axis=0))
    lhs /= scale
    c, _, _, _ = _lstsq(lhs, y, rcond=rcond)
    return (c.T / scale).T


def _polyval(p, x):
    # scipy.signal._polyutils.polyval: Horner from zeros_like(x).
    y = np.zeros_like(x)
    for j in range(p.shape[0]):
        y = y * x + p[j, ...]
    return y


def _polyder(p, m):
    # scipy.signal._savitzky_golay._polyder
    if m == 0:
        result = p
    else:
        n = p.shape[0]
        if n <= m:
            result = np.zeros_like(p[:1, ...])
        else:
            dp = np.array(p[:-m, ...], copy=True)
            for k in range(m):
                rng = np.arange(n - k - 1, m - k - 1, -1, dtype=p.dtype)
                dp *= np.reshape(rng, (n - m,) + (1,) * (p.ndim - 1))
            result = dp
    return result


def savgol_coeffs(window_length, polyorder, deriv=0, delta=1.0, pos=None, use="conv"):
    """``scipy.signal.savgol_coeffs``, scipy's algorithm."""
    if polyorder >= window_length:
        raise ValueError("polyorder must be less than window_length.")
    halflen, rem = divmod(window_length, 2)
    if pos is None:
        if rem == 0:
            pos = halflen - 0.5
        else:
            pos = halflen
    if not (0 <= pos < window_length):
        raise ValueError("pos must be nonnegative and less than window_length.")
    if use not in ["conv", "dot"]:
        raise ValueError("`use` must be 'conv' or 'dot'")
    if deriv > polyorder:
        return np.zeros(window_length, dtype=np.float64)
    x = np.arange(-pos, window_length - pos, dtype=np.float64)
    if use == "conv":
        x = np.flip(x)
    order = np.reshape(np.arange(polyorder + 1, dtype=np.float64), (-1, 1))
    A = x ** order
    y = np.zeros(polyorder + 1, dtype=np.float64)
    y[deriv] = float(math.factorial(deriv)) / (delta ** deriv)
    coeffs, _, _, _ = _lstsq(A, y)
    return coeffs


def _fit_edge(x, window_start, window_stop, interp_start, interp_stop, axis, polyorder, deriv,
              delta, y):
    # scipy.signal._savitzky_golay._fit_edge
    sl = [slice(None)] * x.ndim
    sl[axis] = slice(window_start, window_stop)
    x_edge = x[tuple(sl)]
    if axis == 0 or axis == -x.ndim:
        xx_edge = x_edge
        swapped = False
    else:
        xx_edge = np.swapaxes(x_edge, axis, 0)
        swapped = True
    xx_edge = np.reshape(xx_edge, (xx_edge.shape[0], -1))
    poly_coeffs = _polyfit(np.arange(0, window_stop - window_start, dtype=x.dtype), xx_edge,
                           polyorder)
    if deriv > 0:
        poly_coeffs = _polyder(poly_coeffs, deriv)
    i = np.arange(interp_start - window_start, interp_stop - window_start,
                  dtype=poly_coeffs.dtype)
    values = _polyval(poly_coeffs, np.reshape(i, (-1, 1))) / (delta ** deriv)
    shp = list(y.shape)
    shp[0], shp[axis] = shp[axis], shp[0]
    values = np.reshape(values, (interp_stop - interp_start, *shp[1:]))
    if swapped:
        values = np.swapaxes(values, 0, axis)
    y_slice = [slice(None)] * y.ndim
    y_slice[axis] = slice(interp_start, interp_stop)
    y[tuple(y_slice)] = values
    return y


def _fit_edges_polyfit(x, window_length, polyorder, deriv, delta, axis, y):
    # scipy.signal._savitzky_golay._fit_edges_polyfit
    halflen = window_length // 2
    y = _fit_edge(x, 0, window_length, 0, halflen, axis, polyorder, deriv, delta, y)
    n = x.shape[axis]
    y = _fit_edge(x, n - window_length, n, n - halflen, n, axis, polyorder, deriv, delta, y)
    return y


def savgol_filter(x, window_length, polyorder, deriv=0, delta=1.0, axis=-1, mode="interp",
                  cval=0.0):
    """``scipy.signal.savgol_filter``, scipy's algorithm."""
    if mode not in ["mirror", "constant", "nearest", "interp", "wrap"]:
        raise ValueError("mode must be 'mirror', 'constant', 'nearest' 'wrap' or 'interp'.")
    x = np.asarray(x)
    if x.dtype != np.float64 and x.dtype != np.float32:
        x = x.astype(np.float64)
    coeffs = savgol_coeffs(window_length, polyorder, deriv=deriv, delta=delta)
    if mode == "interp":
        if window_length > x.shape[axis]:
            raise ValueError("If mode is 'interp', window_length must be less than or equal "
                             "to the size of x.")
        y = _ndi.convolve1d(x, coeffs, axis=axis, mode="constant")
        y = _fit_edges_polyfit(x, window_length, polyorder, deriv, delta, axis, y)
    else:
        y = _ndi.convolve1d(x, coeffs, axis=axis, mode=mode, cval=cval)
    return y
