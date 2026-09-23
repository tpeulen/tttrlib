// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_IMAGESTAT_H
#define TTTRLIB_IMAGESTAT_H

// Validation: A/B-TESTED 2026-08-28 -- histogram counts vs a std::map tally
//   and moments vs direct double loops (masked and unmasked, uint8/uint16/
//   float/double), exact for integers and 1e-12 relative for floating; the
//   spot-centroid moment on a synthetic Gaussian lands within 0.01 px of its
//   true centre; in test/cpp/test_image_kernels.cpp.

// Image statistics: the value histogram, and the intensity-weighted image
// moments (m00 = total intensity, m10/m01 = first moments whose ratio is the
// intensity centroid -- the spot-centroid primitive localization can seed a
// Gaussian fit with, m11/m20/m02 = second moments for an orientation or a
// width prior). One O(N) pass each, optional boolean mask, no allocations
// beyond the histogram the caller asked for.
//
// Concept lineage: the histogram-per-region and the masked moments follow
// ermig1979/Simd's Histogram / StatisticMoments (okf/design/simd-port-survey.md)
// -- theirs are label-masked and uint8-only, these are type-generic with a
// boolean mask, re-expressed std-only; the original is MIT,
// (c) 2011-2026 Yermalayeu Ihar, https://github.com/ermig1979/Simd.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tttrlib {

namespace image_stat {

/// Value histogram over [lo, hi] (inclusive): counts how many pixels fall in
/// each of the floor(hi - lo) + 1 bins, pixel value v landing in bin
/// `floor(v - lo)`. Values outside the span are skipped (a span wider than
/// the data is cheap; pass the image's actual dynamic range, not the type's).
/// `mask`, when not null, is a rows x cols uint8 image: zero pixels are
/// skipped. Returns the bin count so a caller can reuse a buffer.
template <class T>
inline size_t value_histogram(const T* src, int rows, int cols,
                              double lo, double hi, std::vector<double>& hist,
                              const std::uint8_t* mask = nullptr) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("value_histogram: empty image");
    if (hi < lo) throw std::runtime_error("value_histogram: hi < lo");
    const size_t bins = static_cast<size_t>(std::floor(hi - lo)) + 1;
    hist.assign(bins, 0.0);
    const size_t n = static_cast<size_t>(rows) * cols;
    for (size_t i = 0; i < n; ++i) {
        if (mask && mask[i] == 0) continue;
        const double v = static_cast<double>(src[i]);
        if (v < lo || v > hi) continue;
        hist[static_cast<size_t>(std::floor(v - lo))] += 1.0;
    }
    return bins;
}

/// Overload for integer pixels: counts uint16 values over a span, same mask
/// contract, exact integer tally.
inline size_t value_histogram(const std::uint16_t* src, int rows, int cols,
                              int lo, int hi, std::vector<double>& hist,
                              const std::uint8_t* mask = nullptr) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("value_histogram: empty image");
    if (hi < lo || lo < 0) throw std::runtime_error("value_histogram: bad span");
    const size_t bins = static_cast<size_t>(hi - lo) + 1;
    hist.assign(bins, 0.0);
    const size_t n = static_cast<size_t>(rows) * cols;
    for (size_t i = 0; i < n; ++i) {
        if (mask && mask[i] == 0) continue;
        const int v = src[i];
        if (v < lo || v > hi) continue;
        hist[static_cast<size_t>(v - lo)] += 1.0;
    }
    return bins;
}

/// Intensity-weighted image moments, computed about the pixel-corner origin
/// (row index r, column index c): m00 = sum(v), m10 = sum(v*c), m01 =
/// sum(v*r), m11 = sum(v*r*c), m20 = sum(v*c^2), m02 = sum(v*r^2). Centroid
/// is (m10/m00, m01/m00) in (x=column, y=row). Values of any numeric type,
/// optional boolean mask; accumulation double.
struct Moments {
    double m00 = 0.0, m10 = 0.0, m01 = 0.0, m11 = 0.0, m20 = 0.0, m02 = 0.0;
    double centroid_x() const { return m10 / m00; }
    double centroid_y() const { return m01 / m00; }
};

template <class T>
inline Moments image_moments(const T* src, int rows, int cols,
                             const std::uint8_t* mask = nullptr) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("image_moments: empty image");
    Moments m;
    for (int r = 0; r < rows; ++r) {
        const T* row = src + static_cast<std::ptrdiff_t>(r) * cols;
        const std::uint8_t* mrow = mask ? mask + static_cast<std::ptrdiff_t>(r) * cols : nullptr;
        const double rd = static_cast<double>(r);
        for (int c = 0; c < cols; ++c) {
            if (mrow && mrow[c] == 0) continue;
            const double v = static_cast<double>(row[c]);
            const double cd = static_cast<double>(c);
            m.m00 += v;
            m.m10 += v * cd;
            m.m01 += v * rd;
            m.m11 += v * rd * cd;
            m.m20 += v * cd * cd;
            m.m02 += v * rd * rd;
        }
    }
    return m;
}

}  // namespace image_stat

}  // namespace tttrlib

#endif  // TTTRLIB_IMAGESTAT_H
