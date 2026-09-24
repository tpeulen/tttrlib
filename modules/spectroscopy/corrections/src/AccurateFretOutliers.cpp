// SPDX-License-Identifier: BSD-3-Clause
// Pre-cleaning of the gating dimensions: impossible and far-out values are
// removed before any scale is computed from the data, so one wild burst can
// neither stretch a standardised axis nor become a population of its own.
#include "AccurateFretMultiDim.h"
#include "AccurateFretDetail.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace tttrlib {

DimensionOutliers flag_dimension_outliers(const std::vector<double>& x, int n_rows,
                                          const std::vector<std::string>& names, double es_lo,
                                          double es_hi, double tau_max, double r_lo, double r_hi,
                                          double fence_k, double quantile) {
    const size_t d = names.size(), n = static_cast<size_t>(n_rows);
    if (d == 0 || x.size() != n * d)
        throw std::invalid_argument("flag_dimension_outliers: x must be (n_rows, len(names))");
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    DimensionOutliers out;
    out.outlier.assign(n, 0);
    out.dimensions = names;
    for (size_t j = 0; j < d; ++j) {
        const std::string& name = names[j];
        double lo = -inf, hi = inf;
        bool open_lo = false, fence = fence_k > 0;
        if (name == "S" || name == "E") {
            lo = es_lo, hi = es_hi, fence = false;
        } else if (name == "tau_d" || name == "tau_a") {
            lo = 0.0, hi = tau_max, open_lo = true;
        } else if (name == "r_d" || name == "r_a") {
            lo = r_lo, hi = r_hi;
        }
        // (1) the physical range; +-inf is out everywhere, NaN is missing
        int n_range = 0;
        std::vector<double> kept;
        std::vector<int> in_range(n, 0);
        for (size_t b = 0; b < n; ++b) {
            const double v = x[b * d + j];
            if (std::isnan(v)) continue;
            const bool ok = std::isfinite(v) && (open_lo ? v > lo : v >= lo) && v <= hi;
            if (!ok) {
                ++n_range;
                out.outlier[b] = 1;
                continue;
            }
            in_range[b] = 1;
            kept.push_back(v);
        }
        // (2) the wide-quantile fence of what passed (1)
        int n_fence = 0;
        double f_lo = nan, f_hi = nan;
        if (fence && kept.size() >= 4) {
            const double ql = std::min(std::max(quantile, 0.0), 0.5);
            const std::vector<double> q = afret_detail::np_quantile(kept, {ql, 1.0 - ql});
            const double w = q[1] - q[0];
            f_lo = q[0] - fence_k * w;
            f_hi = q[1] + fence_k * w;
            for (size_t b = 0; b < n; ++b) {
                if (!in_range[b]) continue;
                const double v = x[b * d + j];
                if (v < f_lo || v > f_hi) {
                    ++n_fence;
                    out.outlier[b] = 1;
                }
            }
        }
        out.n_range.push_back(n_range);
        out.n_fence.push_back(n_fence);
        out.lo.push_back(f_lo);
        out.hi.push_back(f_hi);
    }
    return out;
}

} // namespace tttrlib
