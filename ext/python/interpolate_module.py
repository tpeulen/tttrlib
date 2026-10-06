"""``tttrlib.interpolate`` -- scipy.interpolate's splrep/splev, without scipy.

FITPACK's curfit with given interior knots (``splrep(x, y, t=...)``, the
least-squares spline over fixed knots) and splev, ported from the C FITPACK
scipy 1.18 ships (``Signal.h``). The tck tuple has scipy's layout. Smoothing
splines (``t=None``, ``s>0``, knot placement) are not ported -- nothing
in ChiSurf asks for them.
"""

import numpy as np

import tttrlib as _t

__all__ = ["splrep", "splev"]


def splrep(x, y, w=None, xb=None, xe=None, k=3, task=-1, s=None, t=None, full_output=0,
           per=0, quiet=1):
    """``scipy.interpolate.splrep`` for given knots ``t`` (task -1)."""
    if t is None:
        raise NotImplementedError("tttrlib.interpolate.splrep needs the interior knots t")
    if per:
        raise NotImplementedError("periodic splines are not ported")
    x = np.atleast_1d(np.asarray(x, dtype=np.float64))
    y = np.atleast_1d(np.asarray(y, dtype=np.float64))
    m = len(x)
    w = np.ones(m) if w is None else np.atleast_1d(np.asarray(w, dtype=np.float64))
    if not (len(w) == m == len(y)):
        raise TypeError("Lengths of the first three arguments (x,y,w) must be equal")
    if not (1 <= k <= 5):
        raise TypeError(f"Given degree of the spline (k={k}) is not supported. (1<=k<=5)")
    if m <= k:
        raise TypeError("m > k must hold")
    xb = x[0] if xb is None else float(xb)
    xe = x[-1] if xe is None else float(xe)
    interior = np.ascontiguousarray(np.atleast_1d(np.asarray(t, dtype=np.float64)))
    ier, knots, coeffs, fp = _t.signal_curfit_fixed(x, y, w, xb, xe, int(k), interior)
    if ier == 10:
        raise ValueError(
            "Error on input data: the knots violate the Schoenberg-Whitney conditions or the "
            "arguments are invalid (FITPACK ier=10)"
        )
    if ier != 0:
        raise ValueError(f"FITPACK curfit failed (ier={ier})")
    tck = (knots, coeffs, int(k))
    if full_output:
        return tck, fp, ier, ""
    return tck


def splev(x, tck, der=0, ext=0):
    """``scipy.interpolate.splev`` (value only; ``der=0``)."""
    if der != 0:
        raise NotImplementedError("derivatives are not ported")
    t, c, k = tck
    x = np.asarray(x, dtype=np.float64)
    shape = x.shape
    ier, y = _t.signal_splev(np.ascontiguousarray(t, dtype=np.float64),
                             np.ascontiguousarray(c, dtype=np.float64), int(k),
                             np.ascontiguousarray(x.ravel()), int(ext))
    if ier == 1:
        raise ValueError("Error code returned by splev: x outside the interval and ext=2")
    if ier == 10:
        raise ValueError("Invalid input data")
    return y.reshape(shape) if shape else float(y[0])
