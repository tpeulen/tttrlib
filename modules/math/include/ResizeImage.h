// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_RESIZEIMAGE_H
#define TTTRLIB_RESIZEIMAGE_H

// Validation: A/B-TESTED 2026-08-28 -- area resize vs a direct overlap-weight
//   reference (exact, integer and fractional scales, incl. non-integer up and
//   down), bilinear vs a per-pixel manual evaluation of the same formula, and
//   constant/edge cases; test/cpp/test_image_kernels.cpp.

// Image resampling -- area (box average, the mip/decimation kernel) and
// bilinear (the cheap interpolation for warps and upscales).
//
// Concept lineage: the precomputed index/alpha tables of the bilinear path
// (per-axis x and 1-x pairs resolved once per output row/column, not per
// pixel arithmetic) and the overlap-weight accumulation of the area path
// follow ermig1979/Simd's ResizerBilinear / ResizerArea
// (okf/design/simd-port-survey.md), re-expressed std-only and type-generic;
// the original is MIT, (c) 2011-2026 Yermalayeu Ihar,
// https://github.com/ermig1979/Simd.
//
// Boundary: replicate, consistent with the rank filters. `src` and `dst` must
// not alias. Accumulation is double, so uint16 photon counts rescale without
// integer truncation; the output is cast back to `T`.

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace tttrlib {

namespace resize_image {

namespace detail {

/// Per-output-pixel (index0, index1, alpha) pairs for bilinear: source
/// coordinate `s = (i + 0.5) * scale - 0.5` in pixel centres, clamped.
inline void bilinear_tables(int src_size, int dst_size,
                            std::vector<int>& index, std::vector<double>& alpha) {
    index.resize(static_cast<size_t>(2 * dst_size));
    alpha.resize(static_cast<size_t>(dst_size));
    const double scale = static_cast<double>(src_size) / dst_size;
    for (int i = 0; i < dst_size; ++i) {
        const double s = (i + 0.5) * scale - 0.5;
        const int i0 = std::min(std::max(static_cast<int>(std::floor(s)), 0), src_size - 1);
        const int i1 = std::min(i0 + 1, src_size - 1);
        const double a = std::min(std::max(s - i0, 0.0), 1.0);
        index[static_cast<size_t>(2 * i)] = i0;
        index[static_cast<size_t>(2 * i + 1)] = i1;
        alpha[static_cast<size_t>(i)] = a;
    }
}

}  // namespace detail

/// Per-axis area-resize map: for each output index, the clamped source span
/// and the overlap weights of each span pixel, normalised so the axis sums
/// to 1. Built once per call -- recomputing the fractional overlap per
/// output pixel was ~16x a hardcoded 2x2 loop on the bench; the tables are
/// the whole difference (Simd's resizers do the same, `EstimateIndexAlpha`).
struct AxisMap {
    std::vector<int> start;    ///< first source index of the span
    std::vector<int> count;    ///< span length (>= 1)
    std::vector<double> w;     ///< weights, `count[i]` entries per output index
};

inline AxisMap axis_map(int src_size, int dst_size) {
    AxisMap m;
    m.start.resize(static_cast<size_t>(dst_size));
    m.count.resize(static_cast<size_t>(dst_size));
    size_t total = 0;
    for (int i = 0; i < dst_size; ++i) {
        const double lo = static_cast<double>(i) * src_size / dst_size;
        const double hi = static_cast<double>(i + 1) * src_size / dst_size;
        const int s = std::max(static_cast<int>(std::floor(lo)), 0);
        const int e = std::min(static_cast<int>(std::ceil(hi)), src_size);
        m.start[static_cast<size_t>(i)] = s;
        m.count[static_cast<size_t>(i)] = std::max(e - s, 1);
        total += static_cast<size_t>(m.count[static_cast<size_t>(i)]);
    }
    m.w.resize(total);
    size_t p = 0;
    for (int i = 0; i < dst_size; ++i) {
        const double lo = static_cast<double>(i) * src_size / dst_size;
        const double hi = static_cast<double>(i + 1) * src_size / dst_size;
        const double span = hi - lo;
        const int s = m.start[static_cast<size_t>(i)];
        const int cnt = m.count[static_cast<size_t>(i)];
        for (int k = 0; k < cnt; ++k) {
            const double cov = std::min(static_cast<double>(s + k + 1), hi) -
                               std::max(static_cast<double>(s + k), lo);
            m.w[p++] = std::max(cov, 0.0) / span;
        }
    }
    return m;
}

/// Area (box-average) resize, `rows x cols` -> `drows x dcols`. Each output
/// pixel is the overlap-weighted average of the source rectangle
/// `[dr*rows/drows, (dr+1)*rows/drows) x [dc*cols/dcols, ...)`; the weights
/// are separable, so the kernel is two 1-D passes over precomputed per-axis
/// tables (H into a double buffer, V into `dst`). Fractional scales and
/// upscales fall out of the same tables. Accumulation is double.
template <class T>
inline void resize_area(const T* src, int rows, int cols,
                        T* dst, int drows, int dcols) {
    if (rows <= 0 || cols <= 0 || drows <= 0 || dcols <= 0)
        throw std::runtime_error("resize_area: empty image");
    const AxisMap mx = axis_map(cols, dcols);   // horizontal: cols -> dcols
    const AxisMap my = axis_map(rows, drows);   // vertical: rows -> drows

    // H: every source row shrinks/stretches to dcols
    std::vector<double> tmp(static_cast<size_t>(rows) * dcols);
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(rows >= 64)
#endif
    for (int r = 0; r < rows; ++r) {
        const T* srow = src + static_cast<std::ptrdiff_t>(r) * cols;
        double* drow = tmp.data() + static_cast<std::ptrdiff_t>(r) * dcols;
        size_t wp = 0;
        for (int dc = 0; dc < dcols; ++dc) {
            const int cnt = mx.count[static_cast<size_t>(dc)];
            const int s = mx.start[static_cast<size_t>(dc)];
            double acc = 0.0;
            for (int k = 0; k < cnt; ++k) acc += mx.w[wp + k] * srow[s + k];
            drow[dc] = acc;
            wp += static_cast<size_t>(cnt);
        }
    }

    // offsets into the flat weight table for each output index
    std::vector<size_t> yoff(static_cast<size_t>(drows));
    {
        size_t p = 0;
        for (int dr = 0; dr < drows; ++dr) {
            yoff[static_cast<size_t>(dr)] = p;
            p += static_cast<size_t>(my.count[static_cast<size_t>(dr)]);
        }
    }

    // V: drows rows assembled from tmp's rows
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(drows >= 64)
#endif
    for (int dr = 0; dr < drows; ++dr) {
        T* drow = dst + static_cast<std::ptrdiff_t>(dr) * dcols;
        const int cnt = my.count[static_cast<size_t>(dr)];
        const int s = my.start[static_cast<size_t>(dr)];
        const size_t wbase = yoff[static_cast<size_t>(dr)];
        for (int k = 0; k < cnt; ++k) {
            const double wv = my.w[wbase + static_cast<size_t>(k)];
            const double* trow = tmp.data() + static_cast<std::ptrdiff_t>(s + k) * dcols;
            if (k == 0) {
                for (int dc = 0; dc < dcols; ++dc) drow[dc] = wv * trow[dc];
            } else {
                for (int dc = 0; dc < dcols; ++dc) drow[dc] += wv * trow[dc];
            }
        }
    }
}

/// Bilinear resize, `rows x cols` -> `drows x dcols`. The per-axis
/// (index0, index1, alpha) tables are built once per call; the inner loop is
/// two lerps per output pixel.
template <class T>
inline void resize_bilinear(const T* src, int rows, int cols,
                            T* dst, int drows, int dcols) {
    if (rows <= 0 || cols <= 0 || drows <= 0 || dcols <= 0)
        throw std::runtime_error("resize_bilinear: empty image");
    std::vector<int> ri, ci;
    std::vector<double> ra, ca;
    detail::bilinear_tables(rows, drows, ri, ra);
    detail::bilinear_tables(cols, dcols, ci, ca);
    for (int dr = 0; dr < drows; ++dr) {
        const int r0 = ri[static_cast<size_t>(2 * dr)];
        const int r1 = ri[static_cast<size_t>(2 * dr + 1)];
        const double ar = ra[static_cast<size_t>(dr)];
        const T* row0 = src + static_cast<std::ptrdiff_t>(r0) * cols;
        const T* row1 = src + static_cast<std::ptrdiff_t>(r1) * cols;
        T* drow = dst + static_cast<std::ptrdiff_t>(dr) * dcols;
        for (int dc = 0; dc < dcols; ++dc) {
            const int c0 = ci[static_cast<size_t>(2 * dc)];
            const int c1 = ci[static_cast<size_t>(2 * dc + 1)];
            const double ac = ca[static_cast<size_t>(dc)];
            const double top = row0[c0] + (row0[c1] - row0[c0]) * ac;
            const double bot = row1[c0] + (row1[c1] - row1[c0]) * ac;
            drow[dc] = static_cast<T>(top + (bot - top) * ar);
        }
    }
}

namespace detail {

/// Cubic interpolation weights (Keys, a = -0.5 -- the OpenCV INTER_CUBIC
/// kernel) for a fractional distance d in [0, 1), taps ordered so that tap
/// k weights source index `idx - 1 + k`.
inline void cubic_weights(double d, double w[4]) {
    const double d2 = d * d;
    const double d3 = d2 * d;
    w[0] = -0.5 * d3 + d2 - 0.5 * d;
    w[1] = 1.5 * d3 - 2.5 * d2 + 1.0;
    w[2] = -1.5 * d3 + 2.0 * d2 + 0.5 * d;
    w[3] = 0.5 * d3 - 0.5 * d2;
}

/// Per-axis cubic tables: for each output index, the anchor index and the
/// four weights of taps anchor-1 .. anchor+2, all clamped into the source.
inline void cubic_tables(int src_size, int dst_size,
                          std::vector<int>& index, std::vector<double>& alpha) {
    index.resize(static_cast<size_t>(4 * dst_size));
    alpha.resize(static_cast<size_t>(4 * dst_size));
    const double scale = static_cast<double>(src_size) / dst_size;
    for (int i = 0; i < dst_size; ++i) {
        const double pos = (i + 0.5) * scale - 0.5;
        int idx = static_cast<int>(std::floor(pos));
        double d = pos - idx;
        if (idx <= 0) { idx = 0; d = 0.0; }
        if (idx >= src_size - 1) { idx = src_size - 1; d = 0.0; }
        double w[4];
        cubic_weights(d, w);
        for (int k = 0; k < 4; ++k) {
            int tap = idx - 1 + k;
            tap = tap < 0 ? 0 : (tap > src_size - 1 ? src_size - 1 : tap);
            index[static_cast<size_t>(4 * i + k)] = tap;
            alpha[static_cast<size_t>(4 * i + k)] = w[k];
        }
    }
}

}  // namespace detail

/// Bicubic resize (Keys a = -0.5): sharper upscaling than bilinear -- the
/// publication-figure path, not the analysis path. Per-axis 4-tap tables are
/// built once per call; the kernel is a 4x4 weighted sum per output pixel.
/// Note the weights sum to 1 only up to the kernel's own negative lobes
/// (overshoot near sharp edges is the point of a cubic; values may leave
/// the input range, so integer `T` output clamps by truncation of the cast).
template <class T>
inline void resize_bicubic(const T* src, int rows, int cols,
                           T* dst, int drows, int dcols) {
    if (rows <= 0 || cols <= 0 || drows <= 0 || dcols <= 0)
        throw std::runtime_error("resize_bicubic: empty image");
    std::vector<int> ri, ci;
    std::vector<double> ra, ca;
    detail::cubic_tables(rows, drows, ri, ra);
    detail::cubic_tables(cols, dcols, ci, ca);
    std::vector<double> rowacc(static_cast<size_t>(4) * dcols);
    for (int dr = 0; dr < drows; ++dr) {
        T* drow = dst + static_cast<std::ptrdiff_t>(dr) * dcols;
        for (int k = 0; k < 4; ++k) {
            const T* srow = src + static_cast<std::ptrdiff_t>(ri[static_cast<size_t>(4 * dr + k)]) * cols;
            const double wk = ra[static_cast<size_t>(4 * dr + k)];
            for (int dc = 0; dc < dcols; ++dc) {
                const int c0 = ci[static_cast<size_t>(4 * dc)];
                const int c1 = ci[static_cast<size_t>(4 * dc + 1)];
                const int c2 = ci[static_cast<size_t>(4 * dc + 2)];
                const int c3 = ci[static_cast<size_t>(4 * dc + 3)];
                rowacc[static_cast<size_t>(k) * dcols + dc] = wk *
                    (ca[static_cast<size_t>(4 * dc)] * static_cast<double>(srow[c0]) +
                     ca[static_cast<size_t>(4 * dc + 1)] * static_cast<double>(srow[c1]) +
                     ca[static_cast<size_t>(4 * dc + 2)] * static_cast<double>(srow[c2]) +
                     ca[static_cast<size_t>(4 * dc + 3)] * static_cast<double>(srow[c3]));
            }
        }
        for (int dc = 0; dc < dcols; ++dc)
            drow[dc] = static_cast<T>(rowacc[static_cast<size_t>(dcols) + dc] +
                                      rowacc[static_cast<size_t>(2 * dcols) + dc] +
                                      rowacc[static_cast<size_t>(3 * dcols) + dc] +
                                      rowacc[static_cast<size_t>(0 * dcols) + dc]);
    }
}

}  // namespace resize_image

}  // namespace tttrlib

#endif  // TTTRLIB_RESIZEIMAGE_H
