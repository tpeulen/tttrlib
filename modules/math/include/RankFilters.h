// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_RANKFILTERS_H
#define TTTRLIB_RANKFILTERS_H

// Validation: A/B-TESTED 2026-08-28 -- every filter/shape/edge against a
//   brute-force reference (window gather + std::sort) on random images with
//   and without ties, uint8/uint16/float/double, exact equality (same
//   values, same tie-breaking not required -- order statistics are unique
//   except for the median of an even count, which these odd windows avoid);
//   constant-image invariance and NaN refusal in test/cpp/test_rank_filters.cpp.

// 2D rank filters -- median, min, max, midpoint -- over the fixed windows the
// reference implementations use: 3x3 rhomb, 3x3 square, 5x5 square.
//
// Concept lineage: the filter family, window shapes and edge handling follow
// ermig1979/Simd's MedianFilter/MinFilter/MaxFilter/MidpointFilter
// (okf/design/simd-port-survey.md). Re-expressed std-only, not copied; the
// original is MIT, (c) 2011-2026 Yermalayeu Ihar,
// https://github.com/ermig1979/Simd. One deliberate divergence: Simd packs
// hand-rolled sorting networks per SIMD lane for uint8; here the odd windows
// run through std::sort on a small stack buffer, which stays branch-predictable
// and type-generic (photon-count images are uint16, not uint8), and the tests
// pin exactness against brute force so any future network rewrite has a
// reference to beat.
//
// Edges replicate (the reference's behaviour): out-of-range neighbours sample
// the nearest in-range row/column, so a constant image is a fixed point and
// borders are noisier-free rather than zero-padded.

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace tttrlib {

namespace rank_filters {

/// Window shape: `Rhomb3x3` (centre + 4-neighbours, 5 taps), `Square3x3`
/// (9 taps), `Square5x5` (25 taps).
enum class Shape { Rhomb3x3, Square3x3, Square5x5 };

namespace detail {

/// Gather the window taps at (r, c) with replicated edges into `w`, in a
/// fixed order; returns the tap count.
template <class T>
inline int gather(const T* src, int rows, int cols, int r, int c, Shape shape, T* w) {
    const int rm1 = r > 0 ? r - 1 : 0;
    const int rp1 = r < rows - 1 ? r + 1 : rows - 1;
    const int cm1 = c > 0 ? c - 1 : 0;
    const int cp1 = c < cols - 1 ? c + 1 : cols - 1;
    const T* row_m = src + static_cast<std::ptrdiff_t>(rm1) * cols;
    const T* row_0 = src + static_cast<std::ptrdiff_t>(r) * cols;
    const T* row_p = src + static_cast<std::ptrdiff_t>(rp1) * cols;
    switch (shape) {
        case Shape::Rhomb3x3:
            w[0] = row_m[c]; w[1] = row_0[cm1]; w[2] = row_0[c];
            w[3] = row_0[cp1]; w[4] = row_p[c];
            return 5;
        case Shape::Square3x3: {
            int k = 0;
            const T* ys[3] = {row_m, row_0, row_p};
            const int xs[3] = {cm1, c, cp1};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) w[k++] = ys[i][xs[j]];
            return 9;
        }
        case Shape::Square5x5: {
            int k = 0;
            const int rs[5] = {rm1 > 0 ? (r - 2 > 0 ? r - 2 : 0) : 0,
                               rm1, r, rp1,
                               rp1 < rows - 1 ? (r + 2 < rows ? r + 2 : rows - 1) : rows - 1};
            const int cs[5] = {cm1 > 0 ? (c - 2 > 0 ? c - 2 : 0) : 0,
                               cm1, c, cp1,
                               cp1 < cols - 1 ? (c + 2 < cols ? c + 2 : cols - 1) : cols - 1};
            for (int i = 0; i < 5; ++i) {
                const T* y = src + static_cast<std::ptrdiff_t>(rs[i]) * cols;
                for (int j = 0; j < 5; ++j) w[k++] = y[cs[j]];
            }
            return 25;
        }
    }
    return 0;
}

inline int clampi(int v, int hi) {
    return v < 0 ? 0 : (v > hi ? hi : v);
}

/// Sliding-histogram median (Huang) for a square window, integer pixels.
///
/// The histogram *is* the window multiset, so the median it yields is the
/// same value the sorted gather yields -- exactness is structural, and the
/// brute-force test pins it. `median` carries the current order statistic
/// and `lt` the count of window values below it; each slide moves one column
/// (2R+1 cells), then at most a few histogram bins rebalance -- amortised
/// O(1) instead of a 25-element sort per pixel (measured ~9x on the 5x5
/// 512x512 bench). Clamped edges keep the multiset identical to the gather,
/// duplicates included.
///
/// Rows run in stripes (threaded above the stripe threshold): the scan is
/// sequentially dependent *along* a row, but two row stripes at least
/// `2r+1` apart share no window cells, so each stripe starts its own
/// histogram from scratch and produces identical output to the serial scan.
template <class T>
inline void median_square_hist(const T* src, T* dst, int rows, int cols, int r) {
    constexpr int BINS = (sizeof(T) == 1) ? 256 : 65536;
    const int rank = ((2 * r + 1) * (2 * r + 1)) / 2;

    auto stripe = [&](int r0, int r1) {
        std::vector<int> hist(static_cast<size_t>(BINS), 0);
        long lt = 0;
        int median = 0;
        auto add = [&](int v) { hist[static_cast<size_t>(v)]++; if (v < median) ++lt; };
        auto rem = [&](int v) { hist[static_cast<size_t>(v)]--; if (v < median) --lt; };
        auto rebalance = [&]() {
            while (lt > rank) { --median; lt -= hist[static_cast<size_t>(median)]; }
            while (lt + hist[static_cast<size_t>(median)] <= rank) {
                lt += hist[static_cast<size_t>(median)];
                ++median;
            }
        };
        // first window of the stripe at (r0, 0)
        for (int i = -r; i <= r; ++i)
            for (int j = -r; j <= r; ++j)
                add(src[static_cast<std::ptrdiff_t>(clampi(r0 + i, rows - 1)) * cols +
                        clampi(j, cols - 1)]);
        rebalance();
        dst[static_cast<std::ptrdiff_t>(r0) * cols] = static_cast<T>(median);
        for (int rr = r0; rr < r1; ++rr) {
            for (int cc = 0; cc < cols; ++cc) {
                if (rr == r0 && cc == 0) continue;
                if (cc > 0) {
                    const int drop = clampi(cc - 1 - r, cols - 1);
                    const int take = clampi(cc + r, cols - 1);
                    for (int i = -r; i <= r; ++i) {
                        const std::ptrdiff_t row =
                            static_cast<std::ptrdiff_t>(clampi(rr + i, rows - 1)) * cols;
                        rem(src[row + drop]);
                        add(src[row + take]);
                    }
                } else if (cols > 1) {
                    // The window is still at the previous row's last column:
                    // slide it left to column 0 first (drop right columns, add
                    // left ones), so the vertical slide below happens over the
                    // column span cc=0 actually has. Sliding straight down from
                    // column cols-1 was the first draft's bug -- the brute-force
                    // A/B caught it within one run.
                    for (int c = cols - 1; c > 0; --c) {
                        const int drop = clampi(c + r, cols - 1);
                        const int take = clampi(c - 1 - r, cols - 1);
                        for (int i = -r; i <= r; ++i) {
                            const std::ptrdiff_t row =
                                static_cast<std::ptrdiff_t>(clampi(rr - 1 + i, rows - 1)) * cols;
                            rem(src[row + drop]);
                            add(src[row + take]);
                        }
                    }
                    const int drop = clampi(rr - 1 - r, rows - 1);
                    const int take = clampi(rr + r, rows - 1);
                    for (int j = -r; j <= r; ++j) {
                        const int col = clampi(j, cols - 1);
                        rem(src[static_cast<std::ptrdiff_t>(drop) * cols + col]);
                        add(src[static_cast<std::ptrdiff_t>(take) * cols + col]);
                    }
                } else {
                    // single column: the horizontal span never changes
                    const int drop = clampi(rr - 1 - r, rows - 1);
                    const int take = clampi(rr + r, rows - 1);
                    rem(src[static_cast<std::ptrdiff_t>(drop) * cols]);
                    add(src[static_cast<std::ptrdiff_t>(take) * cols]);
                }
                rebalance();
                dst[static_cast<std::ptrdiff_t>(rr) * cols + cc] = static_cast<T>(median);
            }
        }
    };

#if defined(_OPENMP)
    if (rows >= 128) {
        // stripes of >= 32 rows, boundaries aligned so a stripe's windows
        // never cross into the next stripe's first rows (they may read them,
        // which is safe -- reads only)
        const int stripe_h = 32;
        #pragma omp parallel for schedule(static)
        for (int r0 = 0; r0 < rows; r0 += stripe_h)
            stripe(r0, std::min(r0 + stripe_h, rows));
        return;
    }
#endif
    stripe(0, rows);
}

/// One axis of a separable min or max, row-major both ways: the horizontal
/// pass scans each row, the vertical pass folds the 2r+1 source rows of each
/// output row into an accumulator row. Both are plain row-major sweeps, so
/// no transpose (tried, measured slower than the 9-tap gather it replaced).
template <class T>
inline void extreme_rows(const T* src, T* dst, int rows, int cols, int r, bool is_min) {
    for (int i = 0; i < rows; ++i) {
        const T* row = src + static_cast<std::ptrdiff_t>(i) * cols;
        T* out = dst + static_cast<std::ptrdiff_t>(i) * cols;
        for (int c = 0; c < cols; ++c) {
            T best = row[clampi(c - r, cols - 1)];
            if (is_min) {
                for (int j = -r + 1; j <= r; ++j) {
                    const T v = row[clampi(c + j, cols - 1)];
                    if (v < best) best = v;
                }
            } else {
                for (int j = -r + 1; j <= r; ++j) {
                    const T v = row[clampi(c + j, cols - 1)];
                    if (v > best) best = v;
                }
            }
            out[c] = best;
        }
    }
}

template <class T>
inline void extreme_cols(const T* src, T* dst, int rows, int cols, int r, bool is_min) {
    std::vector<T> acc(static_cast<size_t>(cols));
    for (int i = 0; i < rows; ++i) {
        std::copy(src + static_cast<std::ptrdiff_t>(clampi(i - r, rows - 1)) * cols,
                  src + static_cast<std::ptrdiff_t>(clampi(i - r, rows - 1)) * cols + cols,
                  acc.begin());
        for (int j = -r + 1; j <= r; ++j) {
            const T* row = src + static_cast<std::ptrdiff_t>(clampi(i + j, rows - 1)) * cols;
            if (is_min) {
                for (int c = 0; c < cols; ++c)
                    if (row[c] < acc[c]) acc[c] = row[c];
            } else {
                for (int c = 0; c < cols; ++c)
                    if (row[c] > acc[c]) acc[c] = row[c];
            }
        }
        std::copy(acc.begin(), acc.end(), dst + static_cast<std::ptrdiff_t>(i) * cols);
    }
}

/// Separable extreme over a square window: min and max are separable, so
/// 2(2r+1) comparisons replace the (2r+1)^2-tap gather. Exactness is
/// structural (min of mins), which the brute-force test pins.
template <class T>
inline void extreme_square(const T* src, T* dst, int rows, int cols, int r, bool is_min) {
    std::vector<T> tmp(static_cast<size_t>(rows) * cols);
    extreme_rows(src, tmp.data(), rows, cols, r, is_min);
    extreme_cols(tmp.data(), dst, rows, cols, r, is_min);
}

}  // namespace detail

/// Median of each pixel's window; `src` and `dst` are `rows x cols`, must not
/// alias. Odd windows, so the median is always an actual sample value.
///
/// Integer pixels on a square window run the sliding-histogram path
/// (exact -- the histogram *is* the window multiset); everything else sorts
/// the gathered window.
template <class T>
inline void median_filter(const T* src, T* dst, int rows, int cols, Shape shape) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("median_filter: empty image");
    constexpr bool is_small_int = sizeof(T) <= 2 && !std::is_floating_point<T>::value &&
                                  !std::is_signed<T>::value;
    if (is_small_int && shape != Shape::Rhomb3x3) {
        detail::median_square_hist(src, dst, rows, cols,
                                   shape == Shape::Square3x3 ? 1 : 2);
        return;
    }
    T w[25];
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int n = detail::gather(src, rows, cols, r, c, shape, w);
            std::sort(w, w + n);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] = w[n / 2];
        }
    }
}

/// Minimum of each pixel's window (morphological erosion). The 5x5 square
/// takes the separable two-pass path (min of row-mins, exact); the rhomb and
/// the 3x3 square gather.
template <class T>
inline void min_filter(const T* src, T* dst, int rows, int cols, Shape shape) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("min_filter: empty image");
    // 3x3: the 9-tap gather vectorises better than two passes + a buffer;
    // 5x5: 25 taps vs 10 comparisons, separable wins. Measured, not guessed.
    if (shape == Shape::Square5x5) {
        detail::extreme_square(src, dst, rows, cols, 2, true);
        return;
    }
    T w[25];
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int n = detail::gather(src, rows, cols, r, c, shape, w);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] = *std::min_element(w, w + n);
        }
    }
}

/// Maximum of each pixel's window (morphological dilation). The 5x5 square
/// takes the separable two-pass path (max of row-maxes, exact); the rhomb and
/// the 3x3 square gather.
template <class T>
inline void max_filter(const T* src, T* dst, int rows, int cols, Shape shape) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("max_filter: empty image");
    // 3x3: the 9-tap gather vectorises better than two passes + a buffer;
    // 5x5: 25 taps vs 10 comparisons, separable wins. Measured, not guessed.
    if (shape == Shape::Square5x5) {
        detail::extreme_square(src, dst, rows, cols, 2, false);
        return;
    }
    T w[25];
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int n = detail::gather(src, rows, cols, r, c, shape, w);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] = *std::max_element(w, w + n);
        }
    }
}

/// Midpoint of each pixel's window: `(max + min) / 2`, the reference's
/// midpoint denoise (best on impulse pairs, worst on salt-and-pepper). Square
/// 5x5 window takes the separable two-pass path for both extremes at once;
/// the rhomb and the 3x3 square gather. For integer `T` the result rounds towards zero, matching
/// plain `(a + b) / 2` arithmetic.
template <class T>
inline void midpoint_filter(const T* src, T* dst, int rows, int cols, Shape shape) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("midpoint_filter: empty image");
    if (shape == Shape::Square5x5) {
        std::vector<T> lo(static_cast<size_t>(rows) * cols), hi(lo.size());
        detail::extreme_square(src, lo.data(), rows, cols, 2, true);
        detail::extreme_square(src, hi.data(), rows, cols, 2, false);
        for (size_t i = 0; i < lo.size(); ++i)
            dst[i] = static_cast<T>((lo[i] + hi[i]) / 2);
        return;
    }
    T w[25];
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int n = detail::gather(src, rows, cols, r, c, shape, w);
            const auto mm = std::minmax_element(w, w + n);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] =
                static_cast<T>(( *mm.first + *mm.second ) / 2);
        }
    }
}

}  // namespace rank_filters

}  // namespace tttrlib

#endif  // TTTRLIB_RANKFILTERS_H
