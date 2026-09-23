// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_DRIFTESTIMATOR_H
#define TTTRLIB_DRIFTESTIMATOR_H

// Validation: A/B-TESTED 2026-08-28 -- synthetic random images translated by
//   known integer shifts (all combinations within +/-7 px, several seeds) and
//   bilinear sub-pixel shifts (+/-4 px on a 0.25 px grid): recovered shift
//   within 0.30 px, score monotone in misalignment; constant-image refusal
//   (score 0, shift 0) in test/cpp/test_drift_estimator.cpp.

// Frame-to-frame translation estimator: an image pyramid searched
// coarse-to-fine, minimising the sum of absolute differences, with a
// parabolic sub-pixel refinement on the finest level's error surface.
// The missing primitive for CLSM stacks -- superres takes drift as an input
// today; this estimates it.
//
// Concept lineage: the pyramid + per-level bounded search + 3-tap parabolic
// refinement structure follows ermig1979/Simd's ShiftDetector
// (okf/design/simd-port-survey.md), re-expressed std-only (SAD criterion
// instead of the reference's texture/correlation scoring, plain 2x2 mean
// pyramid instead of its background model). The original is MIT,
// (c) 2011-2026 Yermalayeu Ihar, https://github.com/ermig1979/Simd.
//
// Contract: returns the translation (dx, dy) with
// `ref(r, c) ~= img(r + dy, c + dx)` -- i.e. how to sample `img` so it lands
// on `ref`. A pure-white or flat pair has no translation information: the
// function returns (0, 0) with score 0. The score is the mean absolute
// difference at the optimum, in input units -- compare scores of the same
// pair, not across pairs.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace tttrlib {

namespace drift_estimator {

namespace detail {

/// 2x2 mean reduction with edge replication (the pyramid step).
inline std::vector<double> halve(const std::vector<double>& im, int rows, int cols, int& out_rows, int& out_cols) {
    out_rows = std::max(rows / 2, 1);
    out_cols = std::max(cols / 2, 1);
    std::vector<double> out(static_cast<size_t>(out_rows) * out_cols);
    for (int r = 0; r < out_rows; ++r) {
        const int r0 = std::min(2 * r, rows - 1);
        const int r1 = std::min(2 * r + 1, rows - 1);
        for (int c = 0; c < out_cols; ++c) {
            const int c0 = std::min(2 * c, cols - 1);
            const int c1 = std::min(2 * c + 1, cols - 1);
            out[static_cast<size_t>(r) * out_cols + c] = 0.25 * (im[static_cast<size_t>(r0) * cols + c0] +
                                                                 im[static_cast<size_t>(r0) * cols + c1] +
                                                                 im[static_cast<size_t>(r1) * cols + c0] +
                                                                 im[static_cast<size_t>(r1) * cols + c1]);
        }
    }
    return out;
}

/// Mean absolute difference of `img` sampled at (r + dy, c + dx) with
/// edge clamping, over the central region excluding `margin` pixels.
inline double sad_at(const std::vector<double>& ref, const std::vector<double>& img,
                     int rows, int cols, int dx, int dy, int margin) {
    double acc = 0.0;
    size_t n = 0;
    for (int r = margin; r < rows - margin; ++r) {
        const int ir = std::min(std::max(r + dy, 0), rows - 1);
        const double* rrow = ref.data() + static_cast<std::ptrdiff_t>(r) * cols;
        const double* irow = img.data() + static_cast<std::ptrdiff_t>(ir) * cols;
        for (int c = margin; c < cols - margin; ++c) {
            const int ic = std::min(std::max(c + dx, 0), cols - 1);
            acc += std::abs(rrow[c] - irow[ic]);
            ++n;
        }
    }
    return n ? acc / static_cast<double>(n) : 0.0;
}

}  // namespace detail

/// Estimate the translation between `ref` and `img` (both `rows x cols`,
/// same modality, no aliasing concerns -- both are read only).
///
/// `max_shift` bounds the search at the full resolution (larger values grow
/// the coarse-level search and the margin). On return `dx`, `dy` receive the
/// translation (see the contract above) and `score` the mean absolute
/// difference at that translation; `score == 0` on a flat pair means
/// "no information", not "perfect match". Returns nothing.
///
/// A level count of 0 picks the pyramid depth automatically: halve until the
/// coarsest side is under 64 px, capped so the coarse search stays inside
/// `max_shift`. Sub-pixel accuracy comes from a parabolic fit through the
/// three finest-level SAD taps around the integer optimum; it degrades to
/// the integer shift when the optimum sits at the search boundary.
inline void estimate_shift(const double* ref, const double* img,
                           int rows, int cols, int max_shift,
                           double& dx, double& dy, double& score) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("estimate_shift: empty image");
    if (max_shift < 1) throw std::runtime_error("estimate_shift: max_shift must be >= 1");
    dx = 0.0; dy = 0.0; score = 0.0;

    std::vector<double> ref_p(ref, ref + static_cast<size_t>(rows) * cols);
    std::vector<double> img_p(img, img + static_cast<size_t>(rows) * cols);
    std::vector<int> dims_r, dims_c;
    std::vector<std::vector<double>> pyr_r, pyr_i;
    pyr_r.push_back(std::move(ref_p));
    pyr_i.push_back(std::move(img_p));
    dims_r.push_back(rows);
    dims_c.push_back(cols);

    int levels = 1;
    while (std::min(dims_r.back(), dims_c.back()) > 64 && (1 << (levels - 1)) <= max_shift) {
        int nr, nc;
        pyr_r.push_back(detail::halve(pyr_r.back(), dims_r.back(), dims_c.back(), nr, nc));
        pyr_i.push_back(detail::halve(pyr_i.back(), dims_r.back(), dims_c.back(), nr, nc));
        dims_r.push_back(nr);
        dims_c.push_back(nc);
        ++levels;
    }

    // coarse exhaustive search, seeded with (0, 0) so ties -- every shift of
    // a flat pair, for instance -- resolve to "no shift"
    int L = levels - 1;
    const int coarse_range = std::max(1, (max_shift + (1 << L) - 1) / (1 << L));
    const int coarse_margin = coarse_range + 1;
    int best_dx = 0, best_dy = 0;
    double best = detail::sad_at(pyr_r[L], pyr_i[L], dims_r[L], dims_c[L], 0, 0, coarse_margin);
    for (int dyc = -coarse_range; dyc <= coarse_range; ++dyc) {
        for (int dxc = -coarse_range; dxc <= coarse_range; ++dxc) {
            if (dxc == 0 && dyc == 0) continue;
            const double s = detail::sad_at(pyr_r[L], pyr_i[L], dims_r[L], dims_c[L], dxc, dyc, coarse_margin);
            if (s < best) { best = s; best_dx = dxc; best_dy = dyc; }
        }
    }

    // refine down the pyramid, +/-1 around twice the parent shift
    for (; L-- > 0;) {
        const int factor = 1 << L;
        best_dx *= 2;
        best_dy *= 2;
        const int margin = std::max(2, (max_shift + factor - 1) / factor);
        best = detail::sad_at(pyr_r[L], pyr_i[L], dims_r[L], dims_c[L], best_dx, best_dy, margin);
        for (int ddy = -1; ddy <= 1; ++ddy) {
            for (int ddx = -1; ddx <= 1; ++ddx) {
                if (ddx == 0 && ddy == 0) continue;
                const int tx = best_dx + ddx;
                const int ty = best_dy + ddy;
                const double s = detail::sad_at(pyr_r[L], pyr_i[L], dims_r[L], dims_c[L], tx, ty, margin);
                if (s < best) { best = s; best_dx = tx; best_dy = ty; }
            }
        }
    }

    // parabolic sub-pixel refinement on the finest SAD surface
    dx = best_dx;
    dy = best_dy;
    score = best;
    const int margin0 = 2;
    const double cx = detail::sad_at(pyr_r[0], pyr_i[0], rows, cols, best_dx - 1, best_dy, margin0);
    const double c0 = detail::sad_at(pyr_r[0], pyr_i[0], rows, cols, best_dx, best_dy, margin0);
    const double cx1 = detail::sad_at(pyr_r[0], pyr_i[0], rows, cols, best_dx + 1, best_dy, margin0);
    const double cy0 = detail::sad_at(pyr_r[0], pyr_i[0], rows, cols, best_dx, best_dy - 1, margin0);
    const double cy1 = detail::sad_at(pyr_r[0], pyr_i[0], rows, cols, best_dx, best_dy + 1, margin0);
    const double ddx = cx + cx1 - 2.0 * c0;   // second differences
    const double ddy = cy0 + cy1 - 2.0 * c0;
    // interior minimum only; at the search edge the parabola is meaningless
    if (ddx > 0.0) {
        const double sub = 0.5 * (cx - cx1) / ddx;
        if (std::abs(sub) <= 1.0) dx = best_dx + sub;
    }
    if (ddy > 0.0) {
        const double sub = 0.5 * (cy0 - cy1) / ddy;
        if (std::abs(sub) <= 1.0) dy = best_dy + sub;
    }
}

}  // namespace drift_estimator

}  // namespace tttrlib

#endif  // TTTRLIB_DRIFTESTIMATOR_H
