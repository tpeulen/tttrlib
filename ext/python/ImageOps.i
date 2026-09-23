// SPDX-License-Identifier: BSD-3-Clause
%{
#include "ImageOps.h"
%}

// The image kernels ported from ermig1979/Simd's concepts, exposed to Python
// through the ImageOps.h concrete facade. Conventions, matching Watershed.i:
// images arrive as 2-D typed arrays carrying their own dims; new images,
// histograms and moment vectors leave as ARGOUTVIEWM views (NumPy owns the
// buffer); scalar outs (`dx`, `dy`) use the OUTPUT typemap; every entry
// releases the GIL (long-running, touches no Python object).

// ---- inputs ---------------------------------------------------------------
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* src, int rows, int cols)}
%apply (unsigned short* IN_ARRAY2, int DIM1, int DIM2) {(const std::uint16_t* src, int rows, int cols)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* ref, int ref_rows, int ref_cols)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* img, int img_rows, int img_cols)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* matrix, int matrix_n)}
%apply (unsigned long long* IN_ARRAY1, int DIM1) {(const unsigned long long* integral, int table_n)}
%apply (unsigned char* IN_ARRAY2, int DIM1, int DIM2) {(const std::uint8_t* mask, int mask_rows, int mask_cols)}

// ---- outputs --------------------------------------------------------------
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** dst, int* dst_n)}
%apply (unsigned short** ARGOUTVIEWM_ARRAY1, int* DIM1) {(std::uint16_t** dst, int* dst_n)}
%apply (unsigned long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(unsigned long long** sum, int* sum_n)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** hist, int* n_bins)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out, int* n)}
%apply (double* OUTPUT) {(double* dx)}
%apply (double* OUTPUT) {(double* dy)}

%exception {
    try {
        $action
    } catch (const std::invalid_argument& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    }
}

// argument checks, not operations: kept out of the Python surface
%ignore tttrlib::image_ops::detail::check_dims;
%ignore tttrlib::image_ops::detail::shape_from_int;
%include "ImageOps.h"

#ifdef SWIGPYTHON
%pythoncode %{
_SHAPE_CODES = {"rhomb3x3": 0, "square3x3": 1, "square5x5": 2}

def _shape_code(shape):
    try:
        return _SHAPE_CODES[shape]
    except KeyError:
        raise ValueError(
            "shape must be one of 'rhomb3x3', 'square3x3', 'square5x5', got %r" % (shape,))

def _as_f64_or_u16(image):
    a = np.asarray(image)
    if a.dtype == np.uint16 or a.dtype == np.float64:
        return a
    raise TypeError("image must be float64 or uint16, got %s" % a.dtype)

def _rank_filter(name, image, shape):
    a = _as_f64_or_u16(image)
    code = _shape_code(shape)
    f = globals()[name + ("_f64" if a.dtype == np.float64 else "_u16")]
    return f(a, code).reshape(a.shape)

def median_filter(image, shape="square3x3"):
    """Median of each pixel's window, replicated edges. Exact on integer
    pixels (sliding histogram); `image` is 2-D float64 or uint16."""
    return _rank_filter("median_filter", image, shape)

def min_filter(image, shape="square3x3"):
    """Window minimum (morphological erosion), 2-D float64 or uint16."""
    return _rank_filter("min_filter", image, shape)

def max_filter(image, shape="square3x3"):
    """Window maximum (morphological dilation), 2-D float64 or uint16."""
    return _rank_filter("max_filter", image, shape)

def midpoint_filter(image, shape="square3x3"):
    """Window midpoint ``(min + max) / 2``, 2-D float64 or uint16."""
    return _rank_filter("midpoint_filter", image, shape)

def resize(image, drows, dcols, method="area"):
    """Resample `image` (2-D float64 or uint16) to ``(drows, dcols)``.
    `method` is 'area' (box average, the decimation kernel), 'bilinear'
    or 'bicubic' (Keys a=-0.5, the display upscale)."""
    a = _as_f64_or_u16(image)
    if method not in ("area", "bilinear", "bicubic"):
        raise ValueError("method must be 'area', 'bilinear' or 'bicubic'")
    f = globals()["resize_" + method + ("_f64" if a.dtype == np.float64 else "_u16")]
    return f(a, drows, dcols).reshape(drows, dcols)

def estimate_drift(reference, image, max_shift=8):
    """Frame-to-frame translation: pyramid SAD search with parabolic
    sub-pixel refinement. Returns ``(dx, dy, score)``, where
    ``reference[r, c] ~= image[r + dy, c + dx]`` and `score` is the mean
    absolute difference at the optimum (0 on a flat pair means *no
    information*, not a perfect match)."""
    ref = np.asarray(reference, dtype=np.float64)
    img = np.asarray(image, dtype=np.float64)
    if ref.shape != img.shape:
        raise ValueError("reference and image must have the same shape")
    score, dx, dy = estimate_drift_f64(ref, img, max_shift)
    return dx, dy, score

def image_histogram(image, lo, hi, mask=None):
    """Value histogram over the inclusive span ``[lo, hi]``; values outside
    the span are skipped. `image` is 2-D float64 or uint16; `mask`, when
    given, is a boolean/uint8 array of the same shape (False pixels are
    skipped). Returns the 1-D count array."""
    a = _as_f64_or_u16(image)
    # numpy.i's IN_ARRAY2 cannot take None, so the no-mask path is an
    # all-ones mask (one uint8 per pixel, cheaper than a second entry point)
    m = np.ones(a.shape, dtype=np.uint8) if mask is None else np.asarray(mask, dtype=np.uint8)
    if a.dtype == np.uint16:
        return value_histogram_u16(a, int(lo), int(hi), m)
    return value_histogram_f64(a, float(lo), float(hi), m)

def moments(image, mask=None):
    """Intensity-weighted image moments
    ``(m00, m10, m01, m11, m20, m02)`` about the pixel-corner origin; the
    intensity centroid is ``(m10/m00, m01/m00)`` in (x, y). Optional boolean
    mask of the same shape."""
    a = np.asarray(image, dtype=np.float64)
    m = np.ones(a.shape, dtype=np.uint8) if mask is None else np.asarray(mask, dtype=np.uint8)
    return np.asarray(image_moments_f64(a, m))

def sobel_dx(image):
    """Horizontal Sobel derivative, replicated edges, float64 out."""
    a = np.asarray(image, dtype=np.float64)
    return sobel_dx_f64(a).reshape(a.shape)

def sobel_dy(image):
    """Vertical Sobel derivative, replicated edges, float64 out."""
    a = np.asarray(image, dtype=np.float64)
    return sobel_dy_f64(a).reshape(a.shape)

def laplace(image):
    """Laplace-8 second derivative (8*center minus 8 neighbours),
    replicated edges, float64 out."""
    a = np.asarray(image, dtype=np.float64)
    return laplace8_f64(a).reshape(a.shape)

def gaussian_blur(image, sigma):
    """Fast 3-box blur (sigma-independent cost, replicated edges).
    Not scipy-exact: ~5% of a direct gaussian on band-limited input."""
    a = np.asarray(image, dtype=np.float64)
    return gaussian_blur_f64(a, float(sigma)).reshape(a.shape)

def warp_affine(image, matrix):
    """Affine warp, bilinear, replicated border. `matrix` is the 2x3
    row-major *forward* map (OpenCV ``warpAffine`` convention):
    ``dst(x, y) = src(m^-1 (x, y))``."""
    a = np.asarray(image, dtype=np.float64)
    m = np.asarray(matrix, dtype=np.float64).ravel()
    if m.size != 6:
        raise ValueError("matrix must have 6 entries (2x3 row-major)")
    return warp_affine_f64(a, m).reshape(a.shape)
%}
#endif
