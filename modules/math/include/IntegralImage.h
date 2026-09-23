// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_INTEGRALIMAGE_H
#define TTTRLIB_INTEGRALIMAGE_H

// Validation: A/B-TESTED 2026-08-28 -- sum and sum-of-squares vs a naive
//   per-pixel accumulation on random images (uint8/uint16/int/float/double),
//   exact for integral types and 1e-12 relative for floating; window sums via
//   the integral match direct window sums in test/cpp/test_image_kernels.cpp.

// Integral images (summed-area tables): one linear pass buys O(1) sums over
// any axis-aligned rectangle, the primitive behind local means, variances,
// adaptive thresholds and the fast box/Gaussian filters.
//
// Concept lineage: the (rows+1) x (cols+1) zero-padded layout and the
// sum / sum-of-squares pair follow ermig1979/Simd's Integral
// (okf/design/simd-port-survey.md), re-expressed std-only and type-generic;
// the original is MIT, (c) 2011-2026 Yermalayeu Ihar,
// https://github.com/ermig1979/Simd.
//
// Accumulator choice is the caller's: uint64_t is exact for uint8/uint16
// photon-count images at any realistic size; double accumulates float sources
// with rounding. The width+1 stride makes rectangle sums index-arithmetic
// with no branches.

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace tttrlib {

namespace integral_image {

/// Summed-area table of `src` (rows x cols) into `dst` ((rows+1) x (cols+1),
/// row-major, dst[0..cols] and the left column zero). `U` is the accumulator
/// type; `dst` must be sized `(rows+1)*(cols+1)` and must not alias `src`.
template <class T, class U>
inline void integral_sum(const T* src, int rows, int cols, U* dst) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("integral_sum: empty image");
    for (int c = 0; c <= cols; ++c) dst[c] = U(0);
    for (int r = 0; r < rows; ++r) {
        const T* s = src + static_cast<std::ptrdiff_t>(r) * cols;
        U* d = dst + static_cast<std::ptrdiff_t>(r + 1) * (cols + 1);
        const U* dp = dst + static_cast<std::ptrdiff_t>(r) * (cols + 1);
        d[0] = U(0);
        U run = U(0);
        for (int c = 0; c < cols; ++c) {
            run += static_cast<U>(s[c]);
            d[c + 1] = dp[c + 1] + run;
        }
    }
}

/// Summed-area and squared-sum tables in one pass. `sum` is `(rows+1)x(cols+1)`
/// in `U`; `sqsum` likewise in `V` (use double for float sources so the
/// squares do not overflow small integer accumulators on large images).
template <class T, class U, class V>
inline void integral_sum_sqsum(const T* src, int rows, int cols, U* sum, V* sqsum) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("integral_sum_sqsum: empty image");
    for (int c = 0; c <= cols; ++c) { sum[c] = U(0); sqsum[c] = V(0); }
    for (int r = 0; r < rows; ++r) {
        const T* s = src + static_cast<std::ptrdiff_t>(r) * cols;
        U* d = sum + static_cast<std::ptrdiff_t>(r + 1) * (cols + 1);
        const U* dp = sum + static_cast<std::ptrdiff_t>(r) * (cols + 1);
        V* q = sqsum + static_cast<std::ptrdiff_t>(r + 1) * (cols + 1);
        const V* qp = sqsum + static_cast<std::ptrdiff_t>(r) * (cols + 1);
        d[0] = U(0); q[0] = V(0);
        U run = U(0);
        V runq = V(0);
        for (int c = 0; c < cols; ++c) {
            const V v = static_cast<V>(s[c]);
            run += static_cast<U>(s[c]);
            runq += v * v;
            d[c + 1] = dp[c + 1] + run;
            q[c + 1] = qp[c + 1] + runq;
        }
    }
}

/// Sum of the axis-aligned rectangle [r0, r1] x [c0, c1] (inclusive bounds)
/// from an integral_sum table.
template <class U>
inline U rect_sum(const U* integral, int cols /* = image cols */, int r0, int c0, int r1, int c1) {
    const std::ptrdiff_t stride = cols + 1;
    return integral[static_cast<std::ptrdiff_t>(r1 + 1) * stride + (c1 + 1)] -
           integral[static_cast<std::ptrdiff_t>(r0) * stride + (c1 + 1)] -
           integral[static_cast<std::ptrdiff_t>(r1 + 1) * stride + c0] +
           integral[static_cast<std::ptrdiff_t>(r0) * stride + c0];
}

}  // namespace integral_image

}  // namespace tttrlib

#endif  // TTTRLIB_INTEGRALIMAGE_H
