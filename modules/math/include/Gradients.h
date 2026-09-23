// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_GRADIENTS_H
#define TTTRLIB_GRADIENTS_H

// Validation: A/B-TESTED 2026-08-28 -- every kernel vs a naive clamped 3x3
//   convolution with the same tap weights on random images (uint8/uint16/
//   float/double), exact for the integer inputs and 1e-12 relative for
//   floating; constant image gives exactly zero; in
//   test/cpp/test_gradients_warp.cpp.

// First-order (Sobel) and second-order (Laplace-8) image derivatives with
// replicated edges -- the gradient pair eSRRF-style super-resolution and
// edge-based focus/quality metrics want, and the input side of any
// gradient-magnitude feature.
//
// Concept lineage: the kernels, the clamped-column handling at the left and
// right borders (the first/last output reuses the nearest interior column,
// exactly as the reference's `dst[0] = SobelDx(..., 0, 1)`), and the
// signed-output contract follow ermig1979/Simd's Sobel / Laplace
// (okf/design/simd-port-survey.md), re-expressed std-only and type-generic;
// the original is MIT, (c) 2011-2026 Yermalayeu Ihar,
// https://github.com/ermig1979/Simd.
//
// Output ranges, so callers pick `S` with room: Sobel spans
// +/-4*max|T| (a ±2 tap pair), Laplace-8 spans +/-8*max|T|. For uint8 input
// int16_t is exact and sufficient; for uint16 input use int32_t; floating
// inputs naturally take the same floating type. `src` and `dst` must not
// alias.

#include <cstddef>
#include <stdexcept>

namespace tttrlib {

namespace gradients {

namespace detail {

inline int clamp_edge(int v, int hi) {
    return v < 0 ? 0 : (v > hi ? hi : v);
}

/// One row of a separable-style 3-tap gather with replicated columns:
/// rows (r-1, r, r+1) clamped, columns (c-1, c, c+1) clamped.
template <class T>
inline void rows3(const T* src, int rows, int cols, int r, int c,
                  const T*& s0, const T*& s1, const T*& s2,
                  int& c0, int& c1, int& c2) {
    s0 = src + static_cast<std::ptrdiff_t>(clamp_edge(r - 1, rows - 1)) * cols;
    s1 = src + static_cast<std::ptrdiff_t>(clamp_edge(r, rows - 1)) * cols;
    s2 = src + static_cast<std::ptrdiff_t>(clamp_edge(r + 1, rows - 1)) * cols;
    c0 = clamp_edge(c - 1, cols - 1);
    c1 = clamp_edge(c, cols - 1);
    c2 = clamp_edge(c + 1, cols - 1);
}

}  // namespace detail

/// Horizontal Sobel derivative: the [1 2 1]^T (x) [-1 0 1] kernel, output
/// `dst[r][c]`, signed. Edges replicate rows and clamp columns (the border
/// derivative is one-sided, as the reference computes it).
template <class T, class S>
inline void sobel_dx(const T* src, S* dst, int rows, int cols) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("sobel_dx: empty image");
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(rows >= 128)
#endif
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const T *s0, *s1, *s2;
            int c0, c1, c2;
            detail::rows3(src, rows, cols, r, c, s0, s1, s2, c0, c1, c2);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] =
                static_cast<S>((static_cast<double>(s0[c2]) + 2.0 * static_cast<double>(s1[c2]) +
                                static_cast<double>(s2[c2])) -
                               (static_cast<double>(s0[c0]) + 2.0 * static_cast<double>(s1[c0]) +
                                static_cast<double>(s2[c0])));
        }
    }
}

/// Vertical Sobel derivative: the [-1 0 1]^T (x) [1 2 1] kernel, signed,
/// same edge contract as sobel_dx.
template <class T, class S>
inline void sobel_dy(const T* src, S* dst, int rows, int cols) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("sobel_dy: empty image");
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(rows >= 128)
#endif
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const T *s0, *s1, *s2;
            int c0, c1, c2;
            detail::rows3(src, rows, cols, r, c, s0, s1, s2, c0, c1, c2);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] =
                static_cast<S>((static_cast<double>(s2[c0]) + 2.0 * static_cast<double>(s2[c1]) +
                                static_cast<double>(s2[c2])) -
                               (static_cast<double>(s0[c0]) + 2.0 * static_cast<double>(s0[c1]) +
                                static_cast<double>(s0[c2])));
        }
    }
}

/// Laplace-8: `8*center - sum(neighbours)` over the 3x3 neighbourhood,
/// signed, replicated edges. The un-sharpened second derivative -- the
/// reference's `Laplace` kernel, not the 4-neighbour variant.
template <class T, class S>
inline void laplace8(const T* src, S* dst, int rows, int cols) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("laplace8: empty image");
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(rows >= 128)
#endif
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const T *s0, *s1, *s2;
            int c0, c1, c2;
            detail::rows3(src, rows, cols, r, c, s0, s1, s2, c0, c1, c2);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] =
                static_cast<S>(8.0 * static_cast<double>(s1[c1]) -
                               (static_cast<double>(s0[c0]) + static_cast<double>(s0[c1]) +
                                static_cast<double>(s0[c2]) + static_cast<double>(s1[c0]) +
                                static_cast<double>(s1[c2]) + static_cast<double>(s2[c0]) +
                                static_cast<double>(s2[c1]) + static_cast<double>(s2[c2])));
        }
    }
}

}  // namespace gradients

}  // namespace tttrlib

#endif  // TTTRLIB_GRADIENTS_H
