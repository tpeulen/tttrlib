// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_IMAGEOPS_H
#define TTTRLIB_IMAGEOPS_H

// Validation: A/B-TESTED 2026-08-28 -- every entry point delegates to a
//   kernel whose own A/B lives in test/cpp/test_{rank_filters,image_kernels,
//   drift_estimator,gradients_warp}.cpp; the binding-level parity (numpy in,
//   numpy out, same numbers) is pinned in test/python/misc/test_image_ops.py.

// Concrete, non-template entry points over the header-only image kernels
// (RankFilters / IntegralImage / ResizeImage / FastGaussian / DriftEstimator
// / Gradients / WarpAffine / ImageStat), shaped for the bindings: images in
// as typed pointers with explicit dims, images out as caller-new'd buffers
// with an explicit length each (the ARGOUTVIEWM typemaps wrap those as NumPy
// views). The kernels stay templates; this is the seam that gives them one
// stable ABI per type.
//
// Shape codes (shared by the rank filters): 0 = rhomb 3x3, 1 = square 3x3,
// 2 = square 5x5 -- the reference implementation's three windows.

#include "DriftEstimator.h"
#include "FastGaussian.h"
#include "Gradients.h"
#include "ImageStat.h"
#include "IntegralImage.h"
#include "RankFilters.h"
#include "ResizeImage.h"
#include "WarpAffine.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace tttrlib {
namespace image_ops {

namespace detail {
inline tttrlib::rank_filters::Shape shape_from_int(int shape) {
    switch (shape) {
        case 0: return tttrlib::rank_filters::Shape::Rhomb3x3;
        case 1: return tttrlib::rank_filters::Shape::Square3x3;
        case 2: return tttrlib::rank_filters::Shape::Square5x5;
    }
    throw std::invalid_argument("shape must be 0 (rhomb3x3), 1 (square3x3) or 2 (square5x5)");
}
inline void check_dims(int rows, int cols) {
    if (rows <= 0 || cols <= 0) throw std::invalid_argument("image is empty");
}
/// A buffer for an ARGOUTVIEWM output. malloc, not new[]: the wrappers
/// (numpy.i free_cap, rarrays.i, jsarrays.i) release it with free(), and
/// free() on a new[] allocation is undefined behaviour.
template <typename T>
T* alloc(size_t n) {
    void* p = std::malloc(sizeof(T) * (n ? n : 1));
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
}
}  // namespace detail

/// Rank filters, float64 and uint16 pixels. `*dst` is a malloc'd `rows*cols`
/// buffer; `*dst_n` its length.
#define TTTRLIB_IMAGEOPS_RANK(NAME)                                                        \
inline void NAME##_f64(const double* src, int rows, int cols, int shape,                   \
                       double** dst, int* dst_n) {                                         \
    detail::check_dims(rows, cols);                                                        \
    const auto sh = detail::shape_from_int(shape);  /* throws before the alloc */ \
    *dst = detail::alloc<double>(static_cast<size_t>(rows) * cols);                                   \
    *dst_n = rows * cols;                                                                  \
    tttrlib::rank_filters::NAME(src, *dst, rows, cols, sh);              \
}                                                                                          \
inline void NAME##_u16(const std::uint16_t* src, int rows, int cols, int shape,            \
                       std::uint16_t** dst, int* dst_n) {                                  \
    detail::check_dims(rows, cols);                                                        \
    const auto sh = detail::shape_from_int(shape);  /* throws before the alloc */ \
    *dst = detail::alloc<std::uint16_t>(static_cast<size_t>(rows) * cols);                            \
    *dst_n = rows * cols;                                                                  \
    tttrlib::rank_filters::NAME(src, *dst, rows, cols, sh);              \
}
TTTRLIB_IMAGEOPS_RANK(median_filter)
TTTRLIB_IMAGEOPS_RANK(min_filter)
TTTRLIB_IMAGEOPS_RANK(max_filter)
TTTRLIB_IMAGEOPS_RANK(midpoint_filter)
#undef TTTRLIB_IMAGEOPS_RANK

/// Summed-area table of a uint16 image: `*sum` is a new
/// `(rows+1)*(cols+1)` uint64 buffer in the zero-padded layout.
// The SWIG numpy typemaps are keyed on `unsigned long long`; on LP64 Linux
// `std::uint64_t` is `unsigned long`, a *different* type to the compiler, so
// the generated wrapper failed to compile there. The public parameters are
// `unsigned long long` — the same width everywhere and the exact type the
// typemaps expect — and the narrow cast happens once at the call.
inline void integral_image_u16(const std::uint16_t* src, int rows, int cols,
                               unsigned long long** sum, int* sum_n) {
    detail::check_dims(rows, cols);
    *sum = detail::alloc<unsigned long long>(static_cast<size_t>(rows + 1) * (cols + 1));
    *sum_n = (rows + 1) * (cols + 1);
    tttrlib::integral_image::integral_sum(
        src, rows, cols, reinterpret_cast<std::uint64_t*>(*sum)
    );
}

/// Sum over the inclusive rectangle [r0, r1] x [c0, c1] of an
/// integral_image_u16 table.
inline double rect_sum_u64(const unsigned long long* integral, int table_n, int cols,
                           int r0, int c0, int r1, int c1) {
    (void)table_n;
    return static_cast<double>(
        tttrlib::integral_image::rect_sum(
            reinterpret_cast<const std::uint64_t*>(integral), cols, r0, c0, r1, c1
        ));
}

/// Resamplers: `*dst` is a malloc'd `drows*dcols` buffer.
#define TTTRLIB_IMAGEOPS_RESIZE(NAME)                                                      \
inline void NAME##_f64(const double* src, int rows, int cols,                             \
                       int drows, int dcols, double** dst, int* dst_n) {                   \
    detail::check_dims(rows, cols);                                                        \
    if (drows <= 0 || dcols <= 0) throw std::invalid_argument("output is empty");          \
    *dst = detail::alloc<double>(static_cast<size_t>(drows) * dcols);                                 \
    *dst_n = drows * dcols;                                                                \
    tttrlib::resize_image::NAME(src, rows, cols, *dst, drows, dcols);                               \
}                                                                                          \
inline void NAME##_u16(const std::uint16_t* src, int rows, int cols,                       \
                       int drows, int dcols, std::uint16_t** dst, int* dst_n) {            \
    detail::check_dims(rows, cols);                                                        \
    if (drows <= 0 || dcols <= 0) throw std::invalid_argument("output is empty");          \
    *dst = detail::alloc<std::uint16_t>(static_cast<size_t>(drows) * dcols);                          \
    *dst_n = drows * dcols;                                                                \
    tttrlib::resize_image::NAME(src, rows, cols, *dst, drows, dcols);                               \
}
TTTRLIB_IMAGEOPS_RESIZE(resize_area)
TTTRLIB_IMAGEOPS_RESIZE(resize_bilinear)
TTTRLIB_IMAGEOPS_RESIZE(resize_bicubic)
#undef TTTRLIB_IMAGEOPS_RESIZE

/// 3-box fast gaussian, sigma-independent cost, replicated edges.
inline void gaussian_blur_f64(const double* src, int rows, int cols, double sigma,
                              double** dst, int* dst_n) {
    detail::check_dims(rows, cols);
    if (!(sigma >= 0.0)) throw std::invalid_argument("sigma must be >= 0");   // before the alloc
    *dst = detail::alloc<double>(static_cast<size_t>(rows) * cols);
    *dst_n = rows * cols;
    tttrlib::fast_gaussian::gaussian_blur_fast(src, *dst, rows, cols, sigma);
}

/// Pyramid drift estimation; fills `dx`, `dy`, returns the SAD score.
inline double estimate_drift_f64(const double* ref, int ref_rows, int ref_cols,
                                 const double* img, int img_rows, int img_cols,
                                 int max_shift, double* dx, double* dy) {
    detail::check_dims(ref_rows, ref_cols);
    if (img_rows != ref_rows || img_cols != ref_cols)
        throw std::invalid_argument("reference and image must have the same shape");
    double score;
    tttrlib::drift_estimator::estimate_shift(ref, img, ref_rows, ref_cols, max_shift, *dx, *dy, score);
    return score;
}

/// Derivatives: signed, same extents as the input.
inline void sobel_dx_f64(const double* src, int rows, int cols,
                         double** dst, int* dst_n) {
    detail::check_dims(rows, cols);
    *dst = detail::alloc<double>(static_cast<size_t>(rows) * cols);
    *dst_n = rows * cols;
    tttrlib::gradients::sobel_dx(src, *dst, rows, cols);
}
inline void sobel_dy_f64(const double* src, int rows, int cols,
                         double** dst, int* dst_n) {
    detail::check_dims(rows, cols);
    *dst = detail::alloc<double>(static_cast<size_t>(rows) * cols);
    *dst_n = rows * cols;
    tttrlib::gradients::sobel_dy(src, *dst, rows, cols);
}
inline void laplace8_f64(const double* src, int rows, int cols,
                         double** dst, int* dst_n) {
    detail::check_dims(rows, cols);
    *dst = detail::alloc<double>(static_cast<size_t>(rows) * cols);
    *dst_n = rows * cols;
    tttrlib::gradients::laplace8(src, *dst, rows, cols);
}

/// Affine warp through the inverse of `matrix` (2x3 row-major forward map,
/// OpenCV convention); `matrix` has 6 entries.
inline void warp_affine_f64(const double* src, int rows, int cols,
                            const double* matrix, int matrix_n,
                            double** dst, int* dst_n) {
    detail::check_dims(rows, cols);
    if (matrix_n != 6) throw std::invalid_argument("matrix must have 6 entries (2x3 row-major)");
    *dst = detail::alloc<double>(static_cast<size_t>(rows) * cols);
    *dst_n = rows * cols;
    tttrlib::warp_affine::warp_affine(src, *dst, rows, cols, matrix);
}

/// Value histogram over [lo, hi]; `*hist` receives `floor(hi-lo)+1` counts
/// (`*n_bins` the length). `mask` may be null and must match the image.
inline void value_histogram_f64(const double* src, int rows, int cols,
                                double lo, double hi, const std::uint8_t* mask,
                                int mask_rows, int mask_cols,
                                double** hist, int* n_bins) {
    detail::check_dims(rows, cols);
    if (mask && (mask_rows != rows || mask_cols != cols))
        throw std::invalid_argument("mask shape does not match the image");
    const size_t n = static_cast<size_t>(std::floor(hi - lo)) + 1;
    *hist = detail::alloc<double>(n);
    *n_bins = static_cast<int>(n);
    for (size_t i = 0; i < n; ++i) (*hist)[i] = 0.0;
    const size_t total = static_cast<size_t>(rows) * cols;
    for (size_t i = 0; i < total; ++i) {
        if (mask && mask[i] == 0) continue;
        const double v = src[i];
        if (v < lo || v > hi) continue;
        (*hist)[static_cast<size_t>(std::floor(v - lo))] += 1.0;
    }
}

inline void value_histogram_u16(const std::uint16_t* src, int rows, int cols,
                                int lo, int hi, const std::uint8_t* mask,
                                int mask_rows, int mask_cols,
                                double** hist, int* n_bins) {
    detail::check_dims(rows, cols);
    if (hi < lo || lo < 0) throw std::invalid_argument("bad histogram span");
    if (mask && (mask_rows != rows || mask_cols != cols))
        throw std::invalid_argument("mask shape does not match the image");
    const size_t n = static_cast<size_t>(hi - lo) + 1;
    *hist = detail::alloc<double>(n);
    *n_bins = static_cast<int>(n);
    for (size_t i = 0; i < n; ++i) (*hist)[i] = 0.0;
    const size_t total = static_cast<size_t>(rows) * cols;
    for (size_t i = 0; i < total; ++i) {
        if (mask && mask[i] == 0) continue;
        const int v = src[i];
        if (v < lo || v > hi) continue;
        (*hist)[static_cast<size_t>(v - lo)] += 1.0;
    }
}

/// Intensity-weighted moments; `*out` receives 6 doubles
/// (m00, m10, m01, m11, m20, m02). `mask` may be null and must match.
inline void image_moments_f64(const double* src, int rows, int cols,
                              const std::uint8_t* mask, int mask_rows, int mask_cols,
                              double** out, int* n) {
    detail::check_dims(rows, cols);
    if (mask && (mask_rows != rows || mask_cols != cols))
        throw std::invalid_argument("mask shape does not match the image");
    *out = detail::alloc<double>(6);
    *n = 6;
    const tttrlib::image_stat::Moments m = tttrlib::image_stat::image_moments(src, rows, cols, mask);
    (*out)[0] = m.m00; (*out)[1] = m.m10; (*out)[2] = m.m01;
    (*out)[3] = m.m11; (*out)[4] = m.m20; (*out)[5] = m.m02;
}

}  // namespace image_ops
}  // namespace tttrlib

#endif  // TTTRLIB_IMAGEOPS_H
