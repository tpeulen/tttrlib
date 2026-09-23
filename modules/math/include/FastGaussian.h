// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_FASTGAUSSIAN_H
#define TTTRLIB_FASTGAUSSIAN_H

// Validation: A/B-TESTED 2026-08-28 -- 3-box blur vs a direct O(k) gaussian
//   convolution reference on random images: max abs deviation <= 5% of the
//   signal range for sigma in {0.8, 2, 6} and integral preserved (box and
//   gaussian both sum to 1 within each support), constant-image invariance,
//   in test/cpp/test_image_kernels.cpp.

// Fast gaussian smoothing: three box blurs (separable, O(1) work per pixel
// via prefix sums) whose widths are chosen so the total variance matches the
// requested sigma. Not scipy-exact -- superres carries a private separable
// kernel for that contract -- this is the denoising kernel for imaging paths
// that do not pin a reference implementation.
//
// Concept lineage: the 3-box approximation and the width selection follow
// ermig1979/Simd's GaussianBlur (okf/design/simd-port-survey.md), re-expressed
// std-only; the original is MIT, (c) 2011-2026 Yermalayeu Ihar,
// https://github.com/ermig1979/Simd. The box pass itself is a prefix-sum
// sliding window rather than the reference's running-sum kernel -- same cost
// class, edge-replicate boundary, trivially correct at the borders.
//
// `src` and `dst` must not alias; T is float or double (integer images pass
// through a double buffer in the caller's hands, or use resize/rank kernels).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace tttrlib {

namespace fast_gaussian {

namespace detail {

/// Three odd box widths whose combined variance matches `sigma` (the standard
/// boxes_for_gauss construction: ideal width from 12 sigma^2 / 3, rounded to
/// the two nearest odd values, split by the residual).
inline void boxes_for_gauss(double sigma, int widths[3]) {
    const double w_ideal = std::sqrt((12.0 * sigma * sigma / 3.0) + 1.0);
    int wl = static_cast<int>(std::floor(w_ideal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const double m_ideal = (12.0 * sigma * sigma - 3.0 * static_cast<double>(wl) * wl -
                            12.0 * static_cast<double>(wl) - 9.0) /
                           (-4.0 * static_cast<double>(wl) - 4.0);
    const int m = static_cast<int>(std::lround(m_ideal));
    for (int i = 0; i < 3; ++i) widths[i] = (i < m) ? wl : wu;
    // guard: sigma so small that wl < 3 collapses to identity-ish passes
    for (int i = 0; i < 3; ++i) widths[i] = std::max(widths[i], 1);
}

/// One separable box blur pass (rows then columns), boundary replicated:
/// out-of-range taps sample the border pixel, so a border pixel enters with
/// the full weight of every clipped tap -- the same operator as clamped-index
/// convolution, not a renormalised short window. Running sums, O(1) per
/// pixel, threaded over rows / columns. Reads `src_buf`, writes `dst_buf`,
/// borrows `scratch` as the intermediate (two distinct buffers: a sliding
/// sum over a row being overwritten in place consumes its own tail as
/// "entering" values -- wrong by exactly the pixels just written).
inline void box_blur(const std::vector<double>& src_buf, std::vector<double>& dst_buf,
                     std::vector<double>& scratch, int rows, int cols, int r) {
    if (r < 1) {
        if (&dst_buf != &src_buf) dst_buf = src_buf;
        return;
    }
    const double inv = 1.0 / static_cast<double>(2 * r + 1);
    scratch.resize(static_cast<size_t>(rows) * cols);
    // horizontal: src_buf -> scratch
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(rows >= 32)
#endif
    for (int i = 0; i < rows; ++i) {
        const double* row = src_buf.data() + static_cast<std::ptrdiff_t>(i) * cols;
        double* out = scratch.data() + static_cast<std::ptrdiff_t>(i) * cols;
        double acc = 0.0;
        for (int j = -r; j <= r; ++j)
            acc += row[j < 0 ? 0 : (j > cols - 1 ? cols - 1 : j)];
        for (int c = 0; c < cols; ++c) {
            out[c] = acc * inv;
            const int leave = c - r < 0 ? 0 : (c - r > cols - 1 ? cols - 1 : c - r);
            const int enter = c + r + 1 > cols - 1 ? cols - 1 : c + r + 1;
            acc += row[enter] - row[leave];
        }
    }
    // vertical: scratch -> dst_buf
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(cols >= 32)
#endif
    for (int c = 0; c < cols; ++c) {
        double acc = 0.0;
        for (int j = -r; j <= r; ++j)
            acc += scratch[static_cast<std::ptrdiff_t>(j < 0 ? 0 : (j > rows - 1 ? rows - 1 : j)) * cols + c];
        for (int i = 0; i < rows; ++i) {
            dst_buf[static_cast<std::ptrdiff_t>(i) * cols + c] = acc * inv;
            const int leave = i - r < 0 ? 0 : (i - r > rows - 1 ? rows - 1 : i - r);
            const int enter = i + r + 1 > rows - 1 ? rows - 1 : i + r + 1;
            acc += scratch[static_cast<std::ptrdiff_t>(enter) * cols + c] -
                   scratch[static_cast<std::ptrdiff_t>(leave) * cols + c];
        }
    }
}

}  // namespace detail

/// Gaussian smoothing of a `rows x cols` image with an effective sigma,
/// three box passes, edge-replicated boundary. Cost is independent of sigma.
inline void gaussian_blur_fast(const double* src, double* dst, int rows, int cols, double sigma) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("gaussian_blur_fast: empty image");
    if (sigma < 0.0) throw std::runtime_error("gaussian_blur_fast: negative sigma");
    if (sigma < 0.3) {  // below the box approximation's resolution: identity
        if (src != dst) std::copy(src, src + static_cast<size_t>(rows) * cols, dst);
        return;
    }
    int widths[3];
    detail::boxes_for_gauss(sigma, widths);
    std::vector<double> buf(src, src + static_cast<size_t>(rows) * cols);
    std::vector<double> tmp(static_cast<size_t>(rows) * cols);
    std::vector<double> scratch;
    for (int i = 0; i < 3; ++i)
        detail::box_blur(i % 2 == 0 ? buf : tmp, i % 2 == 0 ? tmp : buf,
                         scratch, rows, cols, widths[i] / 2);
    // three passes: buf->tmp, tmp->buf, buf->tmp -- the result is in tmp
    std::copy(tmp.begin(), tmp.end(), dst);
}

}  // namespace fast_gaussian

}  // namespace tttrlib

#endif  // TTTRLIB_FASTGAUSSIAN_H
