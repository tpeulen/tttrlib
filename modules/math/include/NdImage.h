// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_NDIMAGE_H
#define TTTRLIB_NDIMAGE_H
// Validation: A/B-TESTED 2026-10-05 -- vs scipy.ndimage 1.18.0 on random
//   N-D inputs, every boundary mode, origin and dtype the callers use
//   (test/python/misc/test_ndimage_parity.py): filters, morphology,
//   labelling (numbering included), find_objects, the feature transform
//   (tie-breaking included) and the order 0-5 spline sampler are
//   bit-identical. Needs FP contraction ON (see NdImage.cpp).
//
// NdImage.h -- the part of scipy.ndimage that image analysis actually leans
// on, N-dimensional, std-only.
//
// The algorithms are ports of scipy's C sources (ni_filters.c,
// ni_morphology.c, ni_interpolation.c, ni_splines.c, ni_measure.c and the
// labelling pass of _ni_label.pyx; BSD-3-Clause, Copyright (C) 2003-2005
// Peter J. Verveer and the SciPy developers), not re-derivations: the point of
// moving them is that a caller swapping `scipy.ndimage.x` for
// `tttrlib.ndimage.x` must not see a different number. Three places where
// "the same algorithm" would not have been enough:
//
//   * boundary handling. Separable (1-D) filters extend each line through
//     `extend_line`, which is NI_ExtendLine verbatim; N-D footprint filters
//     map every out-of-range coordinate through `map_filter_index`, which is
//     the switch inside NI_InitFilterOffsets, integer casts included. The two
//     disagree for extensions longer than the line, exactly as scipy's do.
//   * summation order. correlate1d keeps scipy's symmetric/antisymmetric
//     pairing (`(a + b) * w` rather than `a*w + b*w`), and the N-D correlate
//     walks the footprint in C order, skipping weights with |w| <= DBL_EPSILON
//     the way NI_Correlate does.
//   * tie-breaking. The Voronoi feature transform and the label compaction
//     are deterministic orders, not just correct answers; both are copied.
//
// Everything works on C-contiguous float64 (images), uint8 (masks) and
// int32/int64 (labels) buffers plus a shape; dtype handling -- scipy writes
// the output in the input's dtype, pass by pass -- lives in the Python layer
// (`tttrlib.ndimage`), which is the only place that knows the dtype.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tttrlib {
namespace ndimage {

/// Boundary modes, with scipy's integer codes (`_extend_mode_to_code`).
enum Mode : int {
    kNearest = 0,       ///< aaaa|abcd|dddd
    kWrap = 1,          ///< abcd|abcd|abcd (interpolation: period len-1)
    kReflect = 2,       ///< dcba|abcd|dcba
    kMirror = 3,        ///< dcb|abcd|cba
    kConstant = 4,      ///< kkkk|abcd|kkkk
    kGridWrap = 5,      ///< abcd|abcd|abcd (interpolation: period len)
    kGridConstant = 6,  ///< kkkk|abcd|kkkk, interpolating into the padding
};

using Shape = std::vector<std::ptrdiff_t>;
using Origins = std::vector<std::ptrdiff_t>;

/// Number of elements of an array of shape `shape`.
std::ptrdiff_t shape_size(const Shape& shape);

/// Fill the `size_before` and `size_after` padding around a line of
/// `line_length` values that starts at `buffer + size_before`, the way
/// NI_ExtendLine does for `mode` (a single-element line in `kMirror` is
/// treated as `kNearest`).
void extend_line(double* buffer, std::ptrdiff_t line_length, std::ptrdiff_t size_before,
                 std::ptrdiff_t size_after, int mode, double cval);

/// Map coordinate `cc` on an axis of length `len` into `[0, len)` for an N-D
/// footprint filter; sets `outside` (and leaves `cc` unspecified) when the
/// mode is constant and `cc` falls off the array.
std::ptrdiff_t map_filter_index(std::ptrdiff_t cc, std::ptrdiff_t len, int mode, bool& outside);

// --- separable filters --------------------------------------------------------

/// `scipy.ndimage.correlate1d`: `out[i] = sum_j w[j] * in[i + j - nw/2 - origin]`
/// along `axis`, the line extended by `mode`/`cval`.
void correlate1d(const double* in, const Shape& shape, int axis, const double* weights,
                 std::ptrdiff_t n_weights, int mode, double cval, std::ptrdiff_t origin,
                 double* out);

/// Running-sum box mean of `size` samples along `axis` (NI_UniformFilter1D).
void uniform_filter1d(const double* in, const Shape& shape, int axis, std::ptrdiff_t size,
                      int mode, double cval, std::ptrdiff_t origin, double* out);

/// Sliding minimum (`minimum = true`) or maximum along `axis`, Richard Harter's
/// ascending-minima deque (NI_MinOrMaxFilter1D).
void min_or_max_filter1d(const double* in, const Shape& shape, int axis, std::ptrdiff_t size,
                         int mode, double cval, std::ptrdiff_t origin, bool minimum,
                         double* out);

/// The 1-D Gaussian kernel of `gaussian_filter1d` for derivative `order` and
/// half-width `radius`, *already reversed* for use with correlate1d (scipy
/// reverses it because it correlates rather than convolves).
std::vector<double> gaussian_kernel1d(double sigma, int order, std::ptrdiff_t radius);

// --- N-D footprint filters ----------------------------------------------------

/// `scipy.ndimage.correlate` with an N-D weights array of shape `wshape`
/// (same rank as `shape`) and one origin per axis.
void correlate(const double* in, const Shape& shape, const double* weights, const Shape& wshape,
               int mode, double cval, const Origins& origins, double* out);

/// Minimum or maximum over a boolean footprint (NI_MinOrMaxFilter without
/// the grey `structure` offsets). `cval` must already be cast to the
/// caller's dtype.
void min_or_max_filter(const double* in, const Shape& shape, const std::uint8_t* footprint,
                       const Shape& fshape, int mode, double cval, const Origins& origins,
                       bool minimum, double* out);

/// The `rank`-th smallest value over a boolean footprint (NI_RankFilter /
/// NI_Select): `rank = n/2` of the footprint's `n` true elements is the
/// median.
void rank_filter(const double* in, const Shape& shape, const std::uint8_t* footprint,
                 const Shape& fshape, std::ptrdiff_t rank, int mode, double cval,
                 const Origins& origins, double* out);

// --- labelling and objects ----------------------------------------------------

/// `scipy.ndimage.label`: connected components of the nonzero pixels of
/// `in`, connected through the symmetric 3x...x3 `structure`. Labels are
/// numbered 1..n in the order of each component's first pixel in a C-order
/// scan, which is the order scipy's merge-table compaction produces. Returns n.
std::int64_t label(const std::uint8_t* in, const Shape& shape, const std::uint8_t* structure,
                   std::int64_t* out);

/// Bounding boxes of labels 1..`max_label` (`find_objects`): `2 * ndim`
/// entries per label, `[start_0, stop_0, start_1, stop_1, ...]`, or all `-1`
/// for a label that does not occur. Labels outside 1..max_label are ignored.
std::vector<std::int64_t> find_objects(const std::int64_t* labels, const Shape& shape,
                                       std::int64_t max_label);

// --- binary morphology --------------------------------------------------------

/// `scipy.ndimage.binary_erosion` (and, with `invert`, the erosion of the
/// complement that binary_dilation is built on; the caller reflects the
/// structure and origins first, as scipy does). Outside the array the input
/// reads as `border_value`. Where `mask` is given and zero the output keeps
/// the input. `iterations < 1` repeats until nothing changes. Returns whether
/// the last pass changed anything.
bool binary_erosion(const std::uint8_t* in, const Shape& shape, const std::uint8_t* structure,
                    const Shape& sshape, const Origins& origins, int border_value, bool invert,
                    const std::uint8_t* mask, int iterations, std::uint8_t* out);

// --- distance transform ---------------------------------------------------------

/// Exact Euclidean feature transform (Maurer, Qi & Raghavan 2003, scipy's
/// NI_EuclideanFeatureTransform): for every element, the coordinates of the
/// nearest zero element of `in`. `features` has `ndim * size` int32 entries,
/// component-major (the layout of scipy's `(ndim,) + shape` index array).
/// `sampling` (one spacing per axis) may be null.
void euclidean_feature_transform(const std::uint8_t* in, const Shape& shape,
                                 const double* sampling, std::int32_t* features);

// --- B-spline interpolation -------------------------------------------------------

/// In-place B-spline prefilter of order 2..5 along `axis` (NI_SplineFilter1D).
void spline_filter1d(double* data, const Shape& shape, int axis, int order, int mode);

/// Sample the (prefiltered) array at `n_points` coordinates; `coordinates`
/// is `ndim * n_points`, axis-major (`map_coordinates`' layout).
void map_coordinates(const double* in, const Shape& shape, const double* coordinates,
                     std::ptrdiff_t n_points, int order, int mode, double cval, int nprepad,
                     double* out);

/// `scipy.ndimage.shift` on a prefiltered array: `out[i] = in(i - shift)`,
/// output shape = `shape` minus `2 * nprepad` per axis.
void shift(const double* in, const Shape& shape, const double* shifts, int order, int mode,
           double cval, int nprepad, double* out);

}  // namespace ndimage
}  // namespace tttrlib

#endif  // TTTRLIB_NDIMAGE_H
