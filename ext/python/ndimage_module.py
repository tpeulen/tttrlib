"""``tttrlib.ndimage`` -- scipy.ndimage, implemented by tttrlib's C++ kernels.

A drop-in for the part of :mod:`scipy.ndimage` image analysis uses: the
same function names, arguments, defaults and -- the point of it -- the same
numbers. Filters write their result in the input's dtype pass by pass, as
scipy does, so an integer image filtered along two axes is truncated twice
here too; origins, footprint reflection for dilation and convolution, label
numbering and feature-transform tie-breaking all follow scipy.

The kernels are the ``ndi_*`` functions of the extension (``NdImage.h``);
this module only shapes arguments and casts results. Complex input is not
supported (scipy filters complex arrays as two real ones -- do the same).
"""

import operator
from collections.abc import Iterable

import numpy as np

import tttrlib as _t

__all__ = [
    "correlate1d", "convolve1d", "correlate", "convolve",
    "gaussian_filter1d", "gaussian_filter", "gaussian_laplace", "laplace",
    "uniform_filter1d", "uniform_filter",
    "minimum_filter1d", "maximum_filter1d", "minimum_filter", "maximum_filter",
    "rank_filter", "median_filter", "percentile_filter",
    "generate_binary_structure", "iterate_structure",
    "label", "find_objects", "sum_labels", "sum", "mean", "center_of_mass",
    "binary_erosion", "binary_dilation", "binary_opening", "binary_closing",
    "binary_fill_holes",
    "grey_erosion", "grey_dilation", "grey_opening", "grey_closing",
    "distance_transform_edt",
    "spline_filter1d", "spline_filter", "map_coordinates", "shift",
]

_MODES = {
    "nearest": 0, "wrap": 1, "reflect": 2, "grid-mirror": 2, "mirror": 3,
    "constant": 4, "grid-wrap": 5, "grid-constant": 6,
}


def _mode_code(mode, is_filter=False):
    try:
        code = _MODES[mode]
    except (KeyError, TypeError):
        raise RuntimeError("boundary mode not supported")
    if is_filter and code == 5:
        return 1
    if is_filter and code == 6:
        return 4
    return code


def _normalize_sequence(value, rank):
    if isinstance(value, str) or not isinstance(value, Iterable):
        return [value] * rank
    value = list(value)
    if len(value) != rank:
        raise RuntimeError("sequence argument must have length equal to input rank")
    return value


def _check_axes(axes, ndim):
    if axes is None:
        return tuple(range(ndim))
    if np.isscalar(axes):
        axes = (operator.index(axes),)
    axes = tuple(operator.index(a) for a in axes)
    out = []
    for a in axes:
        if a < -ndim or a >= ndim:
            raise ValueError("axis %d is out of bounds" % a)
        out.append(a % ndim)
    if len(set(out)) != len(out):
        raise ValueError("axes must be unique")
    return tuple(out)


def _shape(a):
    return np.asarray(a.shape, dtype=np.int64)


def _f64(a):
    return np.ascontiguousarray(a, dtype=np.float64).ravel()


def _out_dtype(output, input):
    """The dtype scipy's `_get_output` would allocate."""
    if output is None:
        return input.dtype
    if isinstance(output, np.ndarray):
        return output.dtype
    return np.dtype(output)


def _store(values, dtype, safe=False):
    """Cast a float64 result into `dtype` the way scipy's C writes it.

    Line filters cast with a plain C conversion; N-D filters with
    `CASE_FILTER_OUT_SAFE`, which wraps negative values into unsigned types
    as ``-(T)(-x)`` instead of relying on undefined behaviour.
    """
    dt = np.dtype(dtype)
    if dt.kind == "f":
        return values.astype(dt)
    if dt.kind == "b":
        return np.trunc(values).astype(np.int64).astype(np.uint8).view(np.bool_)
    if dt.kind == "u" and safe:
        out = np.empty(values.shape, dtype=dt)
        neg = values <= -1
        out[~neg] = values[~neg].astype(dt)
        out[neg] = -((-values[neg]).astype(dt))
        return out
    if dt.kind in "iu":
        return values.astype(dt)
    raise TypeError("unsupported output dtype %s" % dt)


def _finish(result, output):
    if isinstance(output, np.ndarray):
        if output.shape != result.shape:
            raise RuntimeError("output shape not correct")
        output[...] = result
        return output
    return result


def _invalid_origin(origin, lenw):
    return (origin < -(lenw // 2)) or (origin > (lenw - 1) // 2)


# --- separable filters -------------------------------------------------------------


def correlate1d(input, weights, axis=-1, output=None, mode="reflect", cval=0.0, origin=0):
    """1-D correlation along `axis` (scipy.ndimage.correlate1d)."""
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    weights = np.asarray(weights, dtype=np.float64)
    if weights.ndim != 1 or weights.shape[0] < 1:
        raise RuntimeError("no filter weights given")
    axis = _check_axes(axis, input.ndim)[0]
    if _invalid_origin(origin, len(weights)):
        raise ValueError("Invalid origin; origin must satisfy -(len(weights) // 2) <= origin <= "
                         "(len(weights)-1) // 2")
    dtype = _out_dtype(output, input)
    res = _t.ndi_correlate1d(_f64(input), _shape(input), int(axis), np.ascontiguousarray(weights),
                             _mode_code(mode), float(cval), int(origin))
    return _finish(_store(res.reshape(input.shape), dtype), output)


def convolve1d(input, weights, axis=-1, output=None, mode="reflect", cval=0.0, origin=0):
    """1-D convolution along `axis` (scipy.ndimage.convolve1d)."""
    weights = np.asarray(weights, dtype=np.float64)[::-1]
    origin = -origin
    if not len(weights) & 1:
        origin -= 1
    return correlate1d(input, weights, axis, output, mode, cval, origin)


def _gaussian_kernel1d(sigma, order, radius):
    # numpy, exactly as scipy builds it, so the normalisation sum is numpy's
    if order < 0:
        raise ValueError("order must be non-negative")
    exponent_range = np.arange(order + 1)
    sigma2 = sigma * sigma
    x = np.arange(-radius, radius + 1)
    phi_x = np.exp(-0.5 / sigma2 * x ** 2)
    phi_x = phi_x / phi_x.sum()
    if order == 0:
        return phi_x
    q = np.zeros(order + 1)
    q[0] = 1
    D = np.diag(exponent_range[1:], 1)
    P = np.diag(np.ones(order) / -sigma2, -1)
    Q_deriv = D + P
    for _ in range(order):
        q = Q_deriv.dot(q)
    q = (x[:, None] ** exponent_range).dot(q)
    return q * phi_x


def gaussian_filter1d(input, sigma, axis=-1, order=0, output=None, mode="reflect", cval=0.0,
                      truncate=4.0, *, radius=None):
    """1-D Gaussian filter (scipy.ndimage.gaussian_filter1d)."""
    sd = float(sigma)
    lw = int(truncate * sd + 0.5)
    if radius is not None:
        lw = radius
    if not isinstance(lw, (int, np.integer)) or lw < 0:
        raise ValueError("Radius must be a nonnegative integer.")
    weights = _gaussian_kernel1d(sigma, order, lw)[::-1]
    return correlate1d(input, weights, axis, output, mode, cval, 0)


def gaussian_filter(input, sigma, order=0, output=None, mode="reflect", cval=0.0, truncate=4.0,
                    *, radius=None, axes=None):
    """N-D Gaussian filter as a sequence of 1-D passes (scipy.ndimage.gaussian_filter)."""
    input = np.asarray(input)
    dtype = _out_dtype(output, input)
    axes = _check_axes(axes, input.ndim)
    n = len(axes)
    orders = _normalize_sequence(order, n)
    sigmas = _normalize_sequence(sigma, n)
    modes = _normalize_sequence(mode, n)
    radiuses = _normalize_sequence(radius, n)
    passes = [(axes[i], sigmas[i], orders[i], modes[i], radiuses[i]) for i in range(n)
              if sigmas[i] > 1e-15]
    if not passes:
        return _finish(input.astype(dtype, copy=True), output)
    current = input
    for axis, s, o, m, r in passes:
        current = gaussian_filter1d(current, s, axis, o, dtype, m, cval, truncate, radius=r)
    return _finish(current, output)


def laplace(input, output=None, mode="reflect", cval=0.0, *, axes=None):
    """Discrete Laplace with the [1, -2, 1] stencil (scipy.ndimage.laplace)."""
    def derivative2(inp, axis, out, m, c):
        return correlate1d(inp, [1, -2, 1], axis, out, m, c, 0)
    return _generic_laplace(input, derivative2, output, mode, cval, axes)


def _generic_laplace(input, derivative2, output, mode, cval, axes):
    input = np.asarray(input)
    dtype = _out_dtype(output, input)
    axes = _check_axes(axes, input.ndim)
    if not axes:
        return _finish(input.astype(dtype, copy=True), output)
    modes = _normalize_sequence(mode, len(axes))
    result = derivative2(input, axes[0], dtype, modes[0], cval)
    for ii in range(1, len(axes)):
        tmp = derivative2(input, axes[ii], dtype, modes[ii], cval)
        result += tmp
    return _finish(result, output)


def gaussian_laplace(input, sigma, output=None, mode="reflect", cval=0.0, *, axes=None, **kwargs):
    """Laplacian of Gaussian (scipy.ndimage.gaussian_laplace)."""
    input = np.asarray(input)
    axes = _check_axes(axes, input.ndim)
    sigma = _normalize_sequence(sigma, len(axes))
    if len(axes) < input.ndim:
        full = [0] * input.ndim
        for s, ax in zip(sigma, axes):
            full[ax] = s
        sigma = full

    def derivative2(inp, axis, out, m, c):
        order = [0] * inp.ndim
        order[axis] = 2
        return gaussian_filter(inp, sigma, order, out, m, c, **kwargs)

    return _generic_laplace(input, derivative2, output, mode, cval, axes)


def uniform_filter1d(input, size, axis=-1, output=None, mode="reflect", cval=0.0, origin=0):
    """Running mean of `size` samples along `axis` (scipy.ndimage.uniform_filter1d)."""
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    axis = _check_axes(axis, input.ndim)[0]
    if size < 1:
        raise RuntimeError("incorrect filter size")
    if (size // 2 + origin < 0) or (size // 2 + origin >= size):
        raise ValueError("invalid origin")
    dtype = _out_dtype(output, input)
    res = _t.ndi_uniform_filter1d(_f64(input), _shape(input), int(axis), int(size), _mode_code(mode),
                                  float(cval), int(origin))
    return _finish(_store(res.reshape(input.shape), dtype), output)


def uniform_filter(input, size=3, output=None, mode="reflect", cval=0.0, origin=0, *, axes=None):
    """N-D box mean as 1-D passes (scipy.ndimage.uniform_filter)."""
    input = np.asarray(input)
    dtype = _out_dtype(output, input)
    axes = _check_axes(axes, input.ndim)
    n = len(axes)
    sizes = _normalize_sequence(size, n)
    origins = _normalize_sequence(origin, n)
    modes = _normalize_sequence(mode, n)
    passes = [(axes[i], sizes[i], origins[i], modes[i]) for i in range(n) if sizes[i] > 1]
    if not passes:
        return _finish(input.astype(dtype, copy=True), output)
    current = input
    for axis, s, o, m in passes:
        current = uniform_filter1d(current, int(s), axis, dtype, m, cval, o)
    return _finish(current, output)


def _min_or_max_1d(input, size, axis, output, mode, cval, origin, minimum):
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    axis = _check_axes(axis, input.ndim)[0]
    if size < 1:
        raise RuntimeError("incorrect filter size")
    if (size // 2 + origin < 0) or (size // 2 + origin >= size):
        raise ValueError("invalid origin")
    dtype = _out_dtype(output, input)
    res = _t.ndi_min_or_max_filter1d(_f64(input), _shape(input), int(axis), int(size), _mode_code(mode),
                                     float(cval), int(origin), int(bool(minimum)))
    return _finish(_store(res.reshape(input.shape), dtype), output)


def minimum_filter1d(input, size, axis=-1, output=None, mode="reflect", cval=0.0, origin=0):
    """Sliding minimum along `axis` (scipy.ndimage.minimum_filter1d)."""
    return _min_or_max_1d(input, size, axis, output, mode, cval, origin, True)


def maximum_filter1d(input, size, axis=-1, output=None, mode="reflect", cval=0.0, origin=0):
    """Sliding maximum along `axis` (scipy.ndimage.maximum_filter1d)."""
    return _min_or_max_1d(input, size, axis, output, mode, cval, origin, False)


# --- N-D footprint filters ---------------------------------------------------------


def _expand_footprint(ndim, axes, footprint):
    footprint = np.asarray(footprint)
    if len(axes) < ndim and footprint.ndim == len(axes):
        footprint = np.expand_dims(footprint, tuple(a for a in range(ndim) if a not in axes))
    return footprint


def _expand_origin(ndim, axes, origin):
    num_axes = len(axes)
    origins = _normalize_sequence(origin, num_axes)
    if num_axes < ndim:
        full = [0] * ndim
        for o, ax in zip(origins, axes):
            full[ax] = o
        origins = full
    return [int(o) for o in origins]


def _cast_cval(cval, dtype):
    # scipy's N-D filters compare in the input type: (T)cval
    dt = np.dtype(dtype)
    if dt.kind in "iub":
        return float(np.array(cval).astype(np.float64).astype(dt).astype(np.float64))
    if dt.kind == "f":
        return float(np.float64(cval).astype(dt))
    return float(cval)


def _correlate_or_convolve(input, weights, output, mode, cval, origin, convolution, axes):
    input = np.asarray(input)
    if np.iscomplexobj(input) or np.iscomplexobj(weights):
        raise TypeError("Complex type not supported")
    axes = _check_axes(axes, input.ndim)
    weights = np.asarray(weights, dtype=np.float64)
    weights = _expand_footprint(input.ndim, axes, weights)
    origins = _expand_origin(input.ndim, axes, origin)
    wshape = [ii for ii in weights.shape if ii > 0]
    if len(wshape) != input.ndim:
        raise RuntimeError("weights.ndim (%d) must match len(axes) (%d)" % (len(wshape), len(axes)))
    if convolution:
        weights = weights[tuple([slice(None, None, -1)] * weights.ndim)]
        for ii in range(len(origins)):
            origins[ii] = -origins[ii]
            if not weights.shape[ii] & 1:
                origins[ii] -= 1
    for o, lenw in zip(origins, wshape):
        if _invalid_origin(o, lenw):
            raise ValueError("Invalid origin; origin must satisfy -(weights.shape[k] // 2) <= origin[k] "
                             "<= (weights.shape[k]-1) // 2")
    if not isinstance(mode, str) and isinstance(mode, Iterable):
        raise RuntimeError("A sequence of modes is not supported")
    dtype = _out_dtype(output, input)
    res = _t.ndi_correlate(_f64(input), _shape(input), _f64(weights), _shape(weights), _mode_code(mode),
                           float(cval), np.asarray(origins, dtype=np.int64))
    return _finish(_store(res.reshape(input.shape), dtype, safe=True), output)


def correlate(input, weights, output=None, mode="reflect", cval=0.0, origin=0, *, axes=None):
    """N-D correlation (scipy.ndimage.correlate)."""
    return _correlate_or_convolve(input, weights, output, mode, cval, origin, False, axes)


def convolve(input, weights, output=None, mode="reflect", cval=0.0, origin=0, *, axes=None):
    """N-D convolution (scipy.ndimage.convolve)."""
    return _correlate_or_convolve(input, weights, output, mode, cval, origin, True, axes)


def _min_or_max_filter(input, size, footprint, structure, output, mode, cval, origin, minimum, axes):
    if structure is not None:
        raise NotImplementedError("grey morphology with a non-flat structure is not implemented")
    if footprint is None:
        if size is None:
            raise RuntimeError("no footprint provided")
        separable = True
    else:
        footprint = np.asarray(footprint, dtype=bool)
        if not footprint.any():
            raise ValueError("All-zero footprint is not supported.")
        if footprint.all():
            size = footprint.shape
            footprint = None
            separable = True
        else:
            separable = False
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    dtype = _out_dtype(output, input)
    axes = _check_axes(axes, input.ndim)
    num_axes = len(axes)
    if separable:
        origins = _normalize_sequence(origin, num_axes)
        sizes = _normalize_sequence(size, num_axes)
        modes = _normalize_sequence(mode, num_axes)
        passes = [(axes[i], sizes[i], origins[i], modes[i]) for i in range(num_axes) if sizes[i] > 1]
        if not passes:
            return _finish(input.astype(dtype, copy=True), output)
        current = input
        for axis, s, o, m in passes:
            current = _min_or_max_1d(current, int(s), axis, dtype, m, cval, o, minimum)
        return _finish(current, output)
    footprint = _expand_footprint(input.ndim, axes, footprint)
    origins = _expand_origin(input.ndim, axes, origin)
    fshape = [ii for ii in footprint.shape if ii > 0]
    if len(fshape) != input.ndim:
        raise RuntimeError("footprint.ndim (%d) must match len(axes) (%d)" % (footprint.ndim, len(axes)))
    for o, lenf in zip(origins, fshape):
        if (lenf // 2 + o < 0) or (lenf // 2 + o >= lenf):
            raise ValueError("invalid origin")
    if not isinstance(mode, str) and isinstance(mode, Iterable):
        raise RuntimeError("A sequence of modes is not supported for non-separable footprints")
    res = _t.ndi_min_or_max_filter(_f64(input), _shape(input),
                                   np.ascontiguousarray(footprint, dtype=np.uint8).ravel(),
                                   _shape(footprint), _mode_code(mode), _cast_cval(cval, input.dtype),
                                   np.asarray(origins, dtype=np.int64), int(bool(minimum)))
    return _finish(_store(res.reshape(input.shape), dtype), output)


def minimum_filter(input, size=None, footprint=None, output=None, mode="reflect", cval=0.0, origin=0,
                   *, axes=None):
    """N-D minimum filter (scipy.ndimage.minimum_filter)."""
    return _min_or_max_filter(input, size, footprint, None, output, mode, cval, origin, True, axes)


def maximum_filter(input, size=None, footprint=None, output=None, mode="reflect", cval=0.0, origin=0,
                   *, axes=None):
    """N-D maximum filter (scipy.ndimage.maximum_filter)."""
    return _min_or_max_filter(input, size, footprint, None, output, mode, cval, origin, False, axes)


def _rank_filter(input, rank, size, footprint, output, mode, cval, origin, operation, axes):
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    axes = _check_axes(axes, input.ndim)
    num_axes = len(axes)
    if footprint is None:
        if size is None:
            raise RuntimeError("no footprint or filter size provided")
        sizes = _normalize_sequence(size, num_axes)
        footprint = np.ones(sizes, dtype=bool)
    else:
        footprint = np.asarray(footprint, dtype=bool)
    footprint = _expand_footprint(input.ndim, axes, footprint)
    origins = _expand_origin(input.ndim, axes, origin)
    fshape = [ii for ii in footprint.shape if ii > 0]
    if len(fshape) != input.ndim:
        raise RuntimeError("footprint.ndim (%d) must match len(axes) (%d)" % (footprint.ndim, len(axes)))
    for o, lenf in zip(origins, fshape):
        if (lenf // 2 + o < 0) or (lenf // 2 + o >= lenf):
            raise ValueError("invalid origin")
    filter_size = int(np.where(footprint, 1, 0).sum())
    if operation == "median":
        rank = filter_size // 2
    elif operation == "percentile":
        percentile = rank
        if percentile < 0.0:
            percentile += 100.0
        if percentile < 0 or percentile > 100:
            raise RuntimeError("invalid percentile")
        rank = filter_size - 1 if percentile == 100.0 else int(float(filter_size) * percentile / 100.0)
    if rank < 0:
        rank += filter_size
    if rank < 0 or rank >= filter_size:
        raise RuntimeError("rank not within filter footprint size")
    if rank == 0:
        return minimum_filter(input, None, footprint, output, mode, cval, origins)
    if rank == filter_size - 1:
        return maximum_filter(input, None, footprint, output, mode, cval, origins)
    if not isinstance(mode, str) and isinstance(mode, Iterable):
        raise RuntimeError("A sequence of modes is not supported by non-separable rank filters")
    dtype = _out_dtype(output, input)
    res = _t.ndi_rank_filter(_f64(input), _shape(input), np.ascontiguousarray(footprint, dtype=np.uint8).ravel(),
                             _shape(footprint), int(rank), _mode_code(mode, is_filter=True),
                             _cast_cval(cval, input.dtype), np.asarray(origins, dtype=np.int64))
    return _finish(_store(res.reshape(input.shape), dtype), output)


def rank_filter(input, rank, size=None, footprint=None, output=None, mode="reflect", cval=0.0, origin=0,
                *, axes=None):
    """N-D rank filter (scipy.ndimage.rank_filter)."""
    return _rank_filter(input, operator.index(rank), size, footprint, output, mode, cval, origin, "rank", axes)


def median_filter(input, size=None, footprint=None, output=None, mode="reflect", cval=0.0, origin=0,
                  *, axes=None):
    """N-D median filter (scipy.ndimage.median_filter); even windows take the upper median."""
    return _rank_filter(input, 0, size, footprint, output, mode, cval, origin, "median", axes)


def percentile_filter(input, percentile, size=None, footprint=None, output=None, mode="reflect", cval=0.0,
                      origin=0, *, axes=None):
    """N-D percentile filter (scipy.ndimage.percentile_filter)."""
    return _rank_filter(input, percentile, size, footprint, output, mode, cval, origin, "percentile", axes)


# --- structures, labels, objects ---------------------------------------------------------


def generate_binary_structure(rank, connectivity):
    """3x...x3 structuring element of `rank` dims whose elements lie within `connectivity` steps."""
    if connectivity < 1:
        connectivity = 1
    if rank < 1:
        return np.array(True, dtype=bool)
    output = np.fabs(np.indices([3] * rank) - 1)
    output = np.add.reduce(output, 0)
    return output <= connectivity


def iterate_structure(structure, iterations, origin=None):
    """Dilate a structure with itself `iterations - 1` times (scipy.ndimage.iterate_structure)."""
    structure = np.asarray(structure)
    if iterations < 2:
        return structure.copy()
    ni = iterations - 1
    shape = [ii + ni * (ii - 1) for ii in structure.shape]
    pos = [ni * (structure.shape[ii] // 2) for ii in range(len(shape))]
    slc = tuple(slice(pos[ii], pos[ii] + structure.shape[ii], None) for ii in range(len(shape)))
    out = np.zeros(shape, bool)
    out[slc] = structure != 0
    out = binary_dilation(out, structure, iterations=ni)
    if origin is None:
        return out
    origin = _normalize_sequence(origin, structure.ndim)
    origin = [iterations * o for o in origin]
    return out, origin


def label(input, structure=None, output=None):
    """Label connected nonzero regions (scipy.ndimage.label).

    Returns ``(labels, n)``; regions are numbered by their first pixel in a
    C-order scan, as scipy numbers them. With an `output` array, the labels
    are written there and only ``n`` is returned.
    """
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    if structure is None:
        structure = generate_binary_structure(input.ndim, 1)
    structure = np.asarray(structure, dtype=bool)
    if structure.ndim != input.ndim:
        raise RuntimeError("structure and input must have equal rank")
    for ii in structure.shape:
        if ii != 3:
            raise ValueError("structure dimensions must be equal to 3")
    if input.size == 0:
        res = np.zeros(input.shape, dtype=np.int32)
        n = 0
    else:
        flat, n = _t.ndi_label(np.ascontiguousarray(input != 0, dtype=np.uint8).ravel(), _shape(input),
                               np.ascontiguousarray(structure, dtype=np.uint8).ravel())
        res = flat.reshape(input.shape)
    if isinstance(output, np.ndarray):
        if output.shape != input.shape:
            raise ValueError("output shape not correct")
        output[...] = res
        return int(n)
    dtype = np.int32 if output is None else np.dtype(output)
    return res.astype(dtype), int(n)


def find_objects(input, max_label=0):
    """Bounding-box slices of labels 1..max_label (scipy.ndimage.find_objects)."""
    input = np.asarray(input)
    if not np.issubdtype(input.dtype, np.integer):
        raise TypeError("Only integer types are supported")
    if max_label < 1:
        max_label = int(input.max()) if input.size else 0
    if max_label < 1:
        return []
    boxes = _t.ndi_find_objects(np.ascontiguousarray(input, dtype=np.int64).ravel(), _shape(input),
                                int(max_label)).reshape(int(max_label), input.ndim, 2)
    out = []
    for b in boxes:
        if b[0, 0] < 0:
            out.append(None)
        else:
            out.append(tuple(slice(int(s), int(e), None) for s, e in b))
    return out


def _safely_castable_to_int(dt):
    int_size = np.dtype(int).itemsize
    return ((np.issubdtype(dt, np.signedinteger) and dt.itemsize <= int_size)
            or (np.issubdtype(dt, np.unsignedinteger) and dt.itemsize < int_size))


def _stats(input, labels=None, index=None):
    """(count, sum) per label, numerically as scipy.ndimage._measurements._stats."""
    input = np.asarray(input)
    if labels is None:
        return input.size, input.sum()
    labels = np.asarray(labels)
    input, labels = np.broadcast_arrays(input, labels)
    if index is None:
        vals = input[labels > 0]
        return vals.size, vals.sum()
    if np.isscalar(index):
        vals = input[labels == index]
        return vals.size, vals.sum()
    if (not _safely_castable_to_int(labels.dtype) or labels.min() < 0 or labels.max() > labels.size):
        unique_labels, new_labels = np.unique(labels, return_inverse=True)
        new_labels = np.reshape(new_labels, (-1,))
        counts = np.bincount(new_labels)
        sums = np.bincount(new_labels, weights=input.ravel())
        idxs = np.searchsorted(unique_labels, index)
        idxs[idxs >= unique_labels.size] = 0
        found = unique_labels[idxs] == index
    else:
        counts = np.bincount(labels.ravel())
        sums = np.bincount(labels.ravel(), weights=input.ravel())
        idxs = np.asanyarray(index, np.int_).copy()
        found = (idxs >= 0) & (idxs < counts.size)
        idxs[~found] = 0
    counts = counts[idxs]
    counts[~found] = 0
    sums = sums[idxs]
    sums[~found] = 0
    return counts, sums


def sum_labels(input, labels=None, index=None):
    """Sum of `input` per label (scipy.ndimage.sum_labels)."""
    return _stats(input, labels, index)[1]


sum = sum_labels


def mean(input, labels=None, index=None):
    """Mean of `input` per label (scipy.ndimage.mean)."""
    count, total = _stats(input, labels, index)
    return total / np.asanyarray(count).astype(np.float64)


def center_of_mass(input, labels=None, index=None):
    """Intensity-weighted centroid per label, as tuples (scipy.ndimage.center_of_mass)."""
    input = np.asarray(input)
    normalizer = sum_labels(input, labels, index)
    grids = np.ogrid[[slice(0, i) for i in input.shape]]
    results = [sum_labels(input * grids[d].astype(float), labels, index) / normalizer
               for d in range(input.ndim)]
    if np.isscalar(results[0]):
        return tuple(results)
    return [tuple(v) for v in np.array(results).T]


# --- binary morphology ----------------------------------------------------------------


def _center_is_true(structure, origin):
    structure = np.asarray(structure)
    coor = tuple(oo + ss // 2 for ss, oo in zip(structure.shape, origin))
    return bool(structure[coor])


def _binary_erosion(input, structure, iterations, mask, output, border_value, origin, invert, axes):
    try:
        iterations = operator.index(iterations)
    except TypeError:
        raise TypeError("iterations parameter should be an integer")
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    axes = _check_axes(axes, input.ndim)
    num_axes = len(axes)
    if structure is None:
        structure = generate_binary_structure(num_axes, 1)
    else:
        structure = np.asarray(structure, dtype=bool)
    structure = _expand_footprint(input.ndim, axes, structure)
    if structure.ndim != input.ndim:
        raise RuntimeError("structure and input must have same dimensionality")
    if structure.size < 1:
        raise RuntimeError("structure must not be empty")
    if mask is not None:
        mask = np.asarray(mask)
        if mask.shape != input.shape:
            raise RuntimeError("mask and input must have equal sizes")
        mflat = np.ascontiguousarray(mask != 0, dtype=np.uint8).ravel()
    else:
        mflat = np.zeros(0, dtype=np.uint8)
    origins = _expand_origin(input.ndim, axes, origin)
    res, _changed = _t.ndi_binary_erosion(np.ascontiguousarray(input != 0, dtype=np.uint8).ravel(),
                                          _shape(input), np.ascontiguousarray(structure, dtype=np.uint8).ravel(),
                                          _shape(structure), np.asarray(origins, dtype=np.int64),
                                          int(bool(border_value)), int(bool(invert)), mflat, int(iterations))
    res = res.reshape(input.shape).view(np.bool_)
    if output is None:
        return res.copy()
    if isinstance(output, np.ndarray):
        output[...] = res
        return output
    return res.astype(output)


def binary_erosion(input, structure=None, iterations=1, mask=None, output=None, border_value=0, origin=0,
                   brute_force=False, *, axes=None):
    """Binary erosion (scipy.ndimage.binary_erosion)."""
    return _binary_erosion(input, structure, iterations, mask, output, border_value, origin, False, axes)


def binary_dilation(input, structure=None, iterations=1, mask=None, output=None, border_value=0, origin=0,
                    brute_force=False, *, axes=None):
    """Binary dilation (scipy.ndimage.binary_dilation): erosion of the complement by the reflected structure."""
    input = np.asarray(input)
    axes = _check_axes(axes, input.ndim)
    num_axes = len(axes)
    if structure is None:
        structure = generate_binary_structure(num_axes, 1)
    origin = _normalize_sequence(origin, num_axes)
    structure = np.asarray(structure)
    structure = structure[tuple([slice(None, None, -1)] * structure.ndim)]
    for ii in range(len(origin)):
        origin[ii] = -origin[ii]
        if not structure.shape[ii] & 1:
            origin[ii] -= 1
    return _binary_erosion(input, structure, iterations, mask, output, border_value, origin, True, axes)


def binary_opening(input, structure=None, iterations=1, output=None, origin=0, mask=None, border_value=0,
                   brute_force=False, *, axes=None):
    """Erosion then dilation (scipy.ndimage.binary_opening)."""
    input = np.asarray(input)
    axes = _check_axes(axes, input.ndim)
    if structure is None:
        structure = generate_binary_structure(len(axes), 1)
    tmp = binary_erosion(input, structure, iterations, mask, None, border_value, origin, axes=axes)
    return binary_dilation(tmp, structure, iterations, mask, output, border_value, origin, axes=axes)


def binary_closing(input, structure=None, iterations=1, output=None, origin=0, mask=None, border_value=0,
                   brute_force=False, *, axes=None):
    """Dilation then erosion (scipy.ndimage.binary_closing)."""
    input = np.asarray(input)
    axes = _check_axes(axes, input.ndim)
    if structure is None:
        structure = generate_binary_structure(len(axes), 1)
    tmp = binary_dilation(input, structure, iterations, mask, None, border_value, origin, axes=axes)
    return binary_erosion(tmp, structure, iterations, mask, output, border_value, origin, axes=axes)


def binary_fill_holes(input, structure=None, output=None, origin=0, *, axes=None):
    """Fill the background regions not connected to the border (scipy.ndimage.binary_fill_holes)."""
    input = np.asarray(input)
    mask = np.logical_not(input)
    tmp = np.zeros(mask.shape, bool)
    res = binary_dilation(tmp, structure, -1, mask, None, 1, origin, axes=axes)
    np.logical_not(res, res)
    if isinstance(output, np.ndarray):
        output[...] = res
        return output
    return res


# --- grey morphology (flat footprints) ----------------------------------------------------


def grey_erosion(input, size=None, footprint=None, structure=None, output=None, mode="reflect", cval=0.0,
                 origin=0, *, axes=None):
    """Grey erosion with a flat footprint (scipy.ndimage.grey_erosion)."""
    if size is None and footprint is None and structure is None:
        raise ValueError("size, footprint, or structure must be specified")
    return _min_or_max_filter(input, size, footprint, structure, output, mode, cval, origin, True, axes)


def grey_dilation(input, size=None, footprint=None, structure=None, output=None, mode="reflect", cval=0.0,
                  origin=0, *, axes=None):
    """Grey dilation with a flat footprint (scipy.ndimage.grey_dilation): the footprint is reflected."""
    if size is None and footprint is None and structure is None:
        raise ValueError("size, footprint, or structure must be specified")
    if structure is not None:
        raise NotImplementedError("grey morphology with a non-flat structure is not implemented")
    input = np.asarray(input)
    axes = _check_axes(axes, input.ndim)
    num_axes = len(axes)
    if footprint is not None:
        footprint = np.asarray(footprint)
        footprint = footprint[tuple([slice(None, None, -1)] * footprint.ndim)]
    origin = _normalize_sequence(origin, num_axes)
    for ii in range(len(origin)):
        origin[ii] = -origin[ii]
        if footprint is not None:
            sz = footprint.shape[ii]
        elif size is None:
            sz = 1
        elif not np.isscalar(size):
            sz = size[ii]
        else:
            sz = size
        if not sz & 1:
            origin[ii] -= 1
    return _min_or_max_filter(input, size, footprint, None, output, mode, cval, origin, False, axes)


def grey_opening(input, size=None, footprint=None, structure=None, output=None, mode="reflect", cval=0.0,
                 origin=0, *, axes=None):
    """Grey erosion then dilation (scipy.ndimage.grey_opening)."""
    tmp = grey_erosion(input, size, footprint, structure, None, mode, cval, origin, axes=axes)
    return grey_dilation(tmp, size, footprint, structure, output, mode, cval, origin, axes=axes)


def grey_closing(input, size=None, footprint=None, structure=None, output=None, mode="reflect", cval=0.0,
                 origin=0, *, axes=None):
    """Grey dilation then erosion (scipy.ndimage.grey_closing)."""
    tmp = grey_dilation(input, size, footprint, structure, None, mode, cval, origin, axes=axes)
    return grey_erosion(tmp, size, footprint, structure, output, mode, cval, origin, axes=axes)


# --- distance transform ---------------------------------------------------------------


def distance_transform_edt(input, sampling=None, return_distances=True, return_indices=False, distances=None,
                           indices=None):
    """Exact Euclidean distance transform (scipy.ndimage.distance_transform_edt).

    Distances to the nearest zero element and, on request, that element's
    indices (``(ndim,) + shape`` int32), with scipy's tie-breaking.
    """
    if not return_distances and not return_indices:
        raise RuntimeError("at least one of return_distances/return_indices must be True")
    input = np.atleast_1d(np.where(input, 1, 0).astype(np.int8))
    if sampling is not None:
        sampling = np.asarray(_normalize_sequence(sampling, input.ndim), dtype=np.float64)
    else:
        sampling = np.zeros(0)
    flat = _t.ndi_euclidean_feature_transform(np.ascontiguousarray(input, dtype=np.uint8).ravel(), _shape(input),
                                              np.ascontiguousarray(sampling))
    ft = np.asarray(flat, dtype=np.int32).reshape((input.ndim,) + input.shape)
    result = []
    if return_distances:
        dt = ft - np.indices(input.shape, dtype=ft.dtype)
        dt = dt.astype(np.float64)
        if sampling.size:
            for ii in range(len(sampling)):
                dt[ii, ...] *= sampling[ii]
        np.multiply(dt, dt, dt)
        dt = np.add.reduce(dt, axis=0)
        dt = np.sqrt(dt)
        if distances is not None:
            distances[...] = dt
        else:
            result.append(dt)
    if return_indices:
        if indices is not None:
            indices[...] = ft
        else:
            result.append(ft)
    if len(result) == 2:
        return tuple(result)
    if len(result) == 1:
        return result[0]
    return None


# --- B-spline interpolation -------------------------------------------------------------


def spline_filter1d(input, order=3, axis=-1, output=np.float64, mode="mirror"):
    """B-spline prefilter along one axis (scipy.ndimage.spline_filter1d)."""
    if order < 0 or order > 5:
        raise RuntimeError("spline order not supported")
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    dtype = _out_dtype(output, input)
    if order in (0, 1):
        return _finish(np.array(input).astype(dtype), output)
    axis = _check_axes(axis, input.ndim)[0]
    res = _t.ndi_spline_filter1d(_f64(input), _shape(input), int(axis), int(order), _mode_code(mode))
    return _finish(_store(res.reshape(input.shape), dtype), output)


def spline_filter(input, order=3, output=np.float64, mode="mirror"):
    """B-spline prefilter along every axis (scipy.ndimage.spline_filter)."""
    if order < 2 or order > 5:
        raise RuntimeError("spline order not supported")
    input = np.asarray(input)
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    dtype = _out_dtype(output, input)
    current = input
    if input.ndim > 0:
        for axis in range(input.ndim):
            current = spline_filter1d(current, order, axis, dtype, mode)
    else:
        current = input.astype(dtype)
    return _finish(current, output)


def _prepad_for_spline_filter(input, mode, cval):
    if mode in ("nearest", "grid-constant"):
        npad = 12
        if mode == "grid-constant":
            padded = np.pad(input, npad, mode="constant", constant_values=cval)
        else:
            padded = np.pad(input, npad, mode="edge")
    else:
        npad = 0
        padded = input
    return padded, npad


def _interp_store(values, dtype):
    # scipy's CASE_INTERP_OUT_*: integers round half away from zero and clip
    dt = np.dtype(dtype)
    if dt.kind == "f":
        return values.astype(dt)
    if dt.kind in "iu":
        info = np.iinfo(dt)
        r = np.where(values > 0, values + 0.5, values - 0.5 if dt.kind == "i" else 0)
        return np.clip(np.trunc(r), info.min, info.max).astype(dt)
    if dt.kind == "b":
        return values.astype(np.uint8).view(np.bool_)
    raise TypeError("unsupported output dtype %s" % dt)


def map_coordinates(input, coordinates, output=None, order=3, mode="constant", cval=0.0, prefilter=True):
    """Sample `input` at fractional `coordinates` by B-splines (scipy.ndimage.map_coordinates)."""
    if order < 0 or order > 5:
        raise RuntimeError("spline order not supported")
    input = np.asarray(input)
    coordinates = np.asarray(coordinates)
    if np.iscomplexobj(coordinates) or np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    output_shape = coordinates.shape[1:]
    if input.ndim < 1 or len(output_shape) < 1:
        raise RuntimeError("input and output rank must be > 0")
    if coordinates.shape[0] != input.ndim:
        raise RuntimeError("invalid shape for coordinate array")
    dtype = _out_dtype(output, input)
    if prefilter and order > 1:
        padded, npad = _prepad_for_spline_filter(input, mode, cval)
        filtered = spline_filter(padded, order, output=np.float64, mode=mode)
    else:
        npad = 0
        filtered = input
    res = _t.ndi_map_coordinates(_f64(filtered), _shape(filtered),
                                 np.ascontiguousarray(coordinates, dtype=np.float64).reshape(input.ndim, -1).ravel(),
                                 int(order), _mode_code(mode), float(cval), int(npad))
    return _finish(_interp_store(res.reshape(output_shape), dtype), output)


def shift(input, shift, output=None, order=3, mode="constant", cval=0.0, prefilter=True):
    """Shift an array by spline interpolation (scipy.ndimage.shift)."""
    if order < 0 or order > 5:
        raise RuntimeError("spline order not supported")
    input = np.asarray(input)
    if input.ndim < 1:
        raise RuntimeError("input and output rank must be > 0")
    if np.iscomplexobj(input):
        raise TypeError("Complex type not supported")
    dtype = _out_dtype(output, input)
    if prefilter and order > 1:
        padded, npad = _prepad_for_spline_filter(input, mode, cval)
        filtered = spline_filter(padded, order, output=np.float64, mode=mode)
    else:
        npad = 0
        filtered = input
    shifts = np.asarray(_normalize_sequence(shift, input.ndim), dtype=np.float64)
    res = _t.ndi_shift(_f64(filtered), _shape(filtered), shifts, int(order), _mode_code(mode), float(cval),
                       int(npad))
    return _finish(_interp_store(res.reshape(input.shape), dtype), output)
