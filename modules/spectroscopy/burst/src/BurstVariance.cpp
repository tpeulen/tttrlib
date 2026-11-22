// SPDX-License-Identifier: BSD-3-Clause
#include "BurstVariance.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>

#include "Random.h"
#include "Registry.h"

namespace tttrlib {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

template <class F>
void parallel_for(int n, F&& body) {
    unsigned hc = std::thread::hardware_concurrency();
    int nt = (hc == 0) ? 1 : static_cast<int>(hc);
    if (nt > n) nt = std::max(1, n);
    if (nt <= 1 || n <= 1) {
        for (int i = 0; i < n; ++i) body(i);
        return;
    }
    std::atomic<int> next{0};
    auto worker = [&]() {
        int i;
        while ((i = next.fetch_add(1)) < n) body(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(nt - 1);
    for (int c = 1; c < nt; ++c) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
}

/// Binomial(n, p) by inversion of the CDF with one uniform (expected O(n min(p, 1-p)) steps).
int binomial_inversion(int n, double p, double u) {
    if (n <= 0 || p <= 0.0) return 0;
    if (p >= 1.0) return n;
    const bool flip = p > 0.5;
    const double q = flip ? 1.0 - p : p;
    const double s = q / (1.0 - q);
    double pmf = std::pow(1.0 - q, n);
    double cdf = pmf;
    int k = 0;
    while (u > cdf && k < n) {
        pmf *= s * static_cast<double>(n - k) / static_cast<double>(k + 1);
        ++k;
        cdf += pmf;
        if (pmf <= 0.0 && cdf < u) break;  // underflow guard for very large n
    }
    return flip ? n - k : k;
}

/// Quantile with numpy's default (linear) interpolation; sorts ``x`` in place.
double quantile_linear(std::vector<double>& x, double q) {
    if (x.empty()) return kNaN;
    std::sort(x.begin(), x.end());
    const double h = (static_cast<double>(x.size()) - 1.0) * std::min(std::max(q, 0.0), 1.0);
    const size_t lo = static_cast<size_t>(std::floor(h));
    const size_t hi = std::min(lo + 1, x.size() - 1);
    return x[lo] + (h - static_cast<double>(lo)) * (x[hi] - x[lo]);
}

/// Mean and SD (ddof) of the slice statistic f(a_i, n_i); non-finite values are skipped.
template <class F>
std::pair<double, double> ratio_moments(const std::vector<int>& a, const std::vector<int>& n, int ddof, F&& f) {
    double mean = 0.0, m2 = 0.0;
    int64_t k = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double r = f(static_cast<double>(a[i]), static_cast<double>(n[i]));
        if (!std::isfinite(r)) continue;
        ++k;
        const double d = r - mean;
        mean += d / static_cast<double>(k);
        m2 += d * (r - mean);
    }
    const double dof = static_cast<double>(k - ddof);
    return {mean, dof > 0.0 ? std::sqrt(std::max(m2, 0.0) / dof) : kNaN};
}

}  // namespace

void BurstVariance::set_time_axis(long long* time_axis, int n_time_axis) {
    if (time_axis == nullptr || n_time_axis <= 0) {
        time_axis_.clear();
        return;
    }
    time_axis_.assign(time_axis, time_axis + n_time_axis);
}

void BurstVariance::set_statistic(int statistic, double g_factor, double l1, double l2) {
    if (statistic != RATIO && statistic != ANISOTROPY)
        throw std::invalid_argument("BurstVariance: statistic must be RATIO (0) or ANISOTROPY (1)");
    statistic_ = statistic;
    g_factor_ = g_factor;
    l1_ = l1;
    l2_ = l2;
}

double BurstVariance::transform(double a, double n) const {
    if (statistic_ == RATIO) return a / n;
    const double b = n - a;
    return (a - g_factor_ * b) / ((1.0 - 3.0 * l2_) * a + (2.0 - 3.0 * l1_) * g_factor_ * b);
}

void BurstVariance::set_masks(unsigned char* mask_a, int n_mask_a, unsigned char* mask_b, int n_mask_b) {
    if (mask_a == nullptr || mask_b == nullptr || n_mask_a <= 0 || n_mask_b <= 0) {
        mask_a_.clear();
        mask_b_.clear();
        return;
    }
    if (n_mask_a != n_mask_b)
        throw std::invalid_argument("BurstVariance: the A and B masks differ in length");
    mask_a_.assign(mask_a, mask_a + n_mask_a);
    mask_b_.assign(mask_b, mask_b + n_mask_b);
}

void BurstVariance::compute(
    long long* bursts, int n_bursts, int n_cols,
    int slice_mode, int photons_per_slice, double window_ticks,
    double align_ticks, double align_offset,
    int min_photons, int min_slices, int ddof
) {
    if (slice_mode != PHOTONS && slice_mode != TIME)
        throw std::invalid_argument("BurstVariance: slice_mode must be PHOTONS (0) or TIME (1)");
    if (slice_mode == PHOTONS && photons_per_slice < 1)
        throw std::invalid_argument("BurstVariance: photons_per_slice must be >= 1");
    if (slice_mode == TIME && !(window_ticks > 0.0))
        throw std::invalid_argument("BurstVariance: window_ticks must be > 0 in TIME mode");
    if (min_slices < 1) min_slices = 1;
    if (min_photons < 1) min_photons = 1;
    if (ddof < 0) ddof = 0;

    build_streams();
    if (!time_axis_.empty() && static_cast<int64_t>(time_axis_.size()) != n_total())
        throw std::invalid_argument(
            "BurstVariance: the time axis must have one value per photon of the TTTR stream");

    const size_t nb = (bursts == nullptr || n_bursts < 1 || n_cols != 2) ? 0 : static_cast<size_t>(n_bursts);
    slice_mode_ = slice_mode;
    sd_.assign(nb, kNaN);
    ratio_.assign(nb, kNaN);
    pooled_.assign(nb, kNaN);
    chi2_.assign(nb, kNaN);
    n_slices_.assign(nb, 0);
    mean_photons_.assign(nb, kNaN);
    std::vector<std::vector<int>> per_a(nb), per_n(nb);
    std::vector<std::vector<double>> per_d(nb);
    auto stat = [this](double a, double n) { return transform(a, n); };

    if (n_total() == 0) {  // an empty stream: every burst untested (masks of length 0 clear themselves)
        slice_photons_.clear(); slice_a_.clear(); slice_owner_.clear(); slice_duration_.clear();
        result_.assign(nb, kNaN);
        return;
    }
    const bool masked = !mask_a_.empty();
    if (masked && static_cast<int64_t>(mask_a_.size()) != n_total())
        throw std::invalid_argument(
            "BurstVariance: the masks must have one entry per photon of the TTTR stream");
    const std::vector<uint8_t>& is_a = masked ? mask_a_ : membership(STREAM_A);
    const std::vector<uint8_t>& is_b = masked ? mask_b_ : membership(STREAM_B);
    const std::vector<int64_t>& axis = time_axis_.empty() ? macro_all() : time_axis_;

    for_each_burst(bursts, n_bursts, n_cols, [&](int b, int64_t s, int64_t e) -> double {
        std::vector<int>& sa = per_a[static_cast<size_t>(b)];
        std::vector<int>& sn = per_n[static_cast<size_t>(b)];
        std::vector<double>& sd_dur = per_d[static_cast<size_t>(b)];
        if (slice_mode == PHOTONS) {
            int a = 0, n = 0;
            double t_lo = 0.0, t_hi = 0.0;
            for (int64_t i = s; i <= e; ++i) {
                const bool in_a = is_a[static_cast<size_t>(i)] != 0;
                if (!in_a && is_b[static_cast<size_t>(i)] == 0) continue;
                const double t = static_cast<double>(axis[static_cast<size_t>(i)]);
                if (n == 0) { t_lo = t; t_hi = t; }
                t_lo = std::min(t_lo, t);
                t_hi = std::max(t_hi, t);
                a += in_a ? 1 : 0;
                if (++n == photons_per_slice) {
                    if (n >= min_photons) { sa.push_back(a); sn.push_back(n); sd_dur.push_back(t_hi - t_lo); }
                    a = 0;
                    n = 0;
                }
            }  // the trailing partial slice is dropped
        } else {
            // earliest / latest gated photon on the axis (a folded axis is not monotonic)
            double t_min = std::numeric_limits<double>::infinity();
            double t_max = -std::numeric_limits<double>::infinity();
            for (int64_t i = s; i <= e; ++i) {
                if (is_a[static_cast<size_t>(i)] == 0 && is_b[static_cast<size_t>(i)] == 0) continue;
                const double t = static_cast<double>(axis[static_cast<size_t>(i)]);
                t_min = std::min(t_min, t);
                t_max = std::max(t_max, t);
            }
            if (!(t_max > t_min)) return kNaN;
            double t0 = t_min, t_end = t_max;
            if (align_ticks > 0.0) {
                t0 = (std::floor((t_min + align_offset) / align_ticks) + 1.0) * align_ticks - align_offset;
                t_end = std::floor((t_max + align_offset) / align_ticks) * align_ticks - align_offset;
            }
            const double span = t_end - t0;
            const int64_t k = span > 0.0 ? static_cast<int64_t>(std::floor(span / window_ticks)) : 0;
            if (k < 1) return kNaN;
            std::vector<int> wa(static_cast<size_t>(k), 0), wn(static_cast<size_t>(k), 0);
            for (int64_t i = s; i <= e; ++i) {
                const bool in_a = is_a[static_cast<size_t>(i)] != 0;
                if (!in_a && is_b[static_cast<size_t>(i)] == 0) continue;
                const double w = std::floor((static_cast<double>(axis[static_cast<size_t>(i)]) - t0) / window_ticks);
                if (w < 0.0 || w >= static_cast<double>(k)) continue;
                const size_t j = static_cast<size_t>(w);
                wn[j] += 1;
                wa[j] += in_a ? 1 : 0;
            }
            for (int64_t j = 0; j < k; ++j) {
                if (wn[static_cast<size_t>(j)] >= min_photons) {
                    sa.push_back(wa[static_cast<size_t>(j)]);
                    sn.push_back(wn[static_cast<size_t>(j)]);
                    sd_dur.push_back(window_ticks);
                }
            }
        }
        const int k = static_cast<int>(sn.size());
        n_slices_[static_cast<size_t>(b)] = k;
        if (k < min_slices) {
            sa.clear();
            sn.clear();
            sd_dur.clear();
            return kNaN;
        }
        double sum_a = 0.0, sum_n = 0.0;
        for (int j = 0; j < k; ++j) { sum_a += sa[j]; sum_n += sn[j]; }
        const double p = sum_a / sum_n;
        ratio_[static_cast<size_t>(b)] = p;
        pooled_[static_cast<size_t>(b)] = transform(sum_a, sum_n);
        mean_photons_[static_cast<size_t>(b)] = sum_n / static_cast<double>(k);
        if (p > 0.0 && p < 1.0) {
            double chi2 = 0.0;
            for (int j = 0; j < k; ++j) {
                const double d = sa[j] - sn[j] * p;
                chi2 += d * d / (sn[j] * p * (1.0 - p));
            }
            chi2_[static_cast<size_t>(b)] = chi2;
        }
        const auto mom = ratio_moments(sa, sn, ddof, stat);
        sd_[static_cast<size_t>(b)] = mom.second;
        return mom.first;
    });

    slice_photons_.clear();
    slice_a_.clear();
    slice_owner_.clear();
    slice_duration_.clear();
    for (size_t b = 0; b < nb; ++b) {
        for (size_t j = 0; j < per_n[b].size(); ++j) {
            slice_photons_.push_back(per_n[b][j]);
            slice_a_.push_back(per_a[b][j]);
            slice_duration_.push_back(per_d[b][j]);
            slice_owner_.push_back(static_cast<int>(b));
        }
    }
}

std::vector<int> BurstVariance::get_tested() const {
    std::vector<int> out(sd_.size(), 0);
    for (size_t b = 0; b < sd_.size(); ++b) out[b] = std::isfinite(ratio_[b]) ? 1 : 0;
    return out;
}

std::vector<double> BurstVariance::static_bound(
    double quantile, int n_mc, unsigned int seed, double dispersion, int ddof,
    double background_a, double background_b
) const {
    const size_t nb = ratio_.size();
    std::vector<double> bound(nb, kNaN);
    if (nb == 0 || n_mc < 2) return bound;
    if (ddof < 0) ddof = 0;
    // slice range of every burst in the flat arrays (owners ascend)
    std::vector<size_t> first(nb, 0), count(nb, 0);
    for (size_t j = slice_owner_.size(); j-- > 0;) {
        const size_t b = static_cast<size_t>(slice_owner_[j]);
        first[b] = j;
        ++count[b];
    }
    const double scale = std::sqrt(std::max(dispersion, 0.0));
    const double rate_a = std::max(background_a, 0.0), rate_b = std::max(background_b, 0.0);
    const double rate = rate_a + rate_b;
    const double q_bg = rate > 0.0 ? rate_a / rate : 0.0;
    auto stat = [this](double a, double n) { return transform(a, n); };
    parallel_for(static_cast<int>(nb), [&](int bi) {
        const size_t b = static_cast<size_t>(bi);
        const double p = ratio_[b];
        const size_t k = count[b];
        if (!std::isfinite(p) || k < static_cast<size_t>(ddof) + 1) return;
        std::vector<double> sds(static_cast<size_t>(n_mc));
        std::vector<int> a(k), n(k);
        std::vector<double> prob(k, p);
        for (size_t j = 0; j < k; ++j) n[j] = slice_photons_[first[b] + j];
        if (rate > 0.0) {
            // background fraction of every slice, and the signal ratio that keeps the pooled ratio
            std::vector<double> f(k);
            double sum_n = 0.0, sum_bg = 0.0;
            for (size_t j = 0; j < k; ++j) {
                double d = slice_duration_[first[b] + j];
                if (slice_mode_ == PHOTONS && n[j] > 1) d *= static_cast<double>(n[j]) / (n[j] - 1);
                f[j] = std::min(std::max(rate * d / std::max(n[j], 1), 0.0), 1.0);
                sum_n += n[j];
                sum_bg += f[j] * n[j];
            }
            double p_s = sum_n > sum_bg ? (p * sum_n - q_bg * sum_bg) / (sum_n - sum_bg) : 0.0;
            p_s = std::min(std::max(std::isfinite(p_s) ? p_s : 0.0, 0.0), 1.0);
            for (size_t j = 0; j < k; ++j) prob[j] = f[j] * q_bg + (1.0 - f[j]) * p_s;
        }
        const uint64_t base = static_cast<uint64_t>(b) << 32;
        for (int m = 0; m < n_mc; ++m) {
            for (size_t j = 0; j < k; ++j) {
                const double u = Random::deterministic(seed, base + static_cast<uint64_t>(m) * k + j);
                a[j] = binomial_inversion(n[j], prob[j], u);
            }
            sds[static_cast<size_t>(m)] = ratio_moments(a, n, ddof, stat).second;
        }
        sds.erase(std::remove_if(sds.begin(), sds.end(), [](double v) { return !std::isfinite(v); }), sds.end());
        bound[b] = quantile_linear(sds, quantile) * scale;
    });
    return bound;
}

std::vector<double> BurstVariance::shot_noise_sd(
    const std::vector<double>& ratio, const std::vector<double>& photons, double dispersion
) {
    if (photons.size() != ratio.size() && photons.size() != 1)
        throw std::invalid_argument("BurstVariance::shot_noise_sd: photons must match ratio or be one value");
    std::vector<double> out(ratio.size(), kNaN);
    for (size_t i = 0; i < ratio.size(); ++i) {
        const double n = photons.size() == 1 ? photons[0] : photons[i];
        const double p = ratio[i];
        if (n > 0.0 && std::isfinite(p))
            out[i] = std::sqrt(std::max(dispersion, 0.0) * std::max(p * (1.0 - p), 0.0) / n);
    }
    return out;
}

std::vector<int> BurstVariance::classify(const std::vector<double>& sd, const std::vector<double>& bound) {
    if (sd.size() != bound.size())
        throw std::invalid_argument("BurstVariance::classify: sd and bound differ in length");
    std::vector<int> out(sd.size(), -1);
    for (size_t i = 0; i < sd.size(); ++i) {
        if (!std::isfinite(sd[i]) || !std::isfinite(bound[i])) continue;
        out[i] = sd[i] <= bound[i] ? 1 : 0;
    }
    return out;
}

// ---- registry("operation") entry ------------------------------------------
// Burst variance analysis (the proximity ratio of donor and acceptor photons over burst slices)
// as a pipeline step, next to the class that computes it.
namespace {
const char* const kBvaEntry = R"JSON({
  "name": "bva",
  "api": ["BurstVariance"],
  "label": "Burst Variance Analysis",
  "summary": "BVA (Torella et al. 2011): slices each burst into photon or time windows, computes the proximity ratio per window, reports per burst the mean, the standard deviation and the class against the burst's own static bound (quantile of binomial simulations of its slices).",
  "operation_type": "burst_variance_analysis",
  "data_format": "dstore",
  "row_grain": "burst",
  "kind": "burst_table",
  "inputs": {
    "required": [
      "tttr_photon_stream",
      "burst_selection"
    ],
    "description": "Photon stream for per-window photon colours, burst indices for slicing."
  },
  "outputs": {
    "columns": [
      "Proximity Ratio Mean",
      "SD(Proximity Ratio)",
      "BVA Class"
    ]
  },
  "settings_schema": {
    "type": "object",
    "properties": {
      "photons_per_slice": {
        "type": "integer",
        "default": 5,
        "minimum": 1
      },
      "quantile": {
        "type": "number",
        "default": 0.95,
        "minimum": 0.5,
        "maximum": 1.0
      }
    }
  },
  "can_replay": true
})JSON";
}  // namespace

/// Register this operation's registry entry. Idempotent (a duplicate key is refused).
void register_operation_bva() {
    register_algorithm_json("operation", "bva", kBvaEntry);
}

}  // namespace tttrlib
