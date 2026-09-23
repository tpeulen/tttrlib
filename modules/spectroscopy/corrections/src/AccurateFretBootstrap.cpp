// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretUncertainty.h"
#include "AccurateFretDetail.h"
#include "Random.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// positions 0..size-1 of one resample: given ones are checked, missing ones drawn
// from the counter-based generator (reproducible from seed + draw counter alone)
std::vector<int> positions(const std::vector<int>& given, size_t size, unsigned int seed,
                           unsigned long long& counter) {
    if (!given.empty()) {
        if (given.size() != size)
            throw std::invalid_argument("bootstrap: resampling positions must have the class size");
        for (int v : given)
            if (v < 0 || static_cast<size_t>(v) >= size)
                throw std::invalid_argument("bootstrap: resampling position out of range");
        return given;
    }
    std::vector<int> out(size);
    for (size_t i = 0; i < size; ++i) {
        const double u = Random::deterministic(seed, counter++);
        out[i] = std::min(static_cast<int>(u * static_cast<double>(size)), static_cast<int>(size) - 1);
    }
    return out;
}

template <typename T>
std::vector<T> take(const std::vector<T>& v, const std::vector<size_t>& idx, const std::vector<int>& s) {
    std::vector<T> out(s.size());
    for (size_t i = 0; i < s.size(); ++i) out[i] = v[idx[s[i]]];
    return out;
}

std::vector<size_t> nonzero(const std::vector<int>& m) {
    std::vector<size_t> out;
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i]) out.push_back(i);
    return out;
}

} // namespace

void auto_calibrate_bootstrap(AutoCalibration& st, const AutoCalibrateOptions& o,
                              const std::vector<int>& s_d, const std::vector<int>& s_a,
                              const std::vector<int>& s_f) {
    if (!st.has_split) return;
    const auto& dd = st.data_dd;
    const auto& da = st.data_da;
    const auto& aa = st.data_aa;
    const FretFactors& f = st.factors;
    const std::vector<size_t> idx_d = nonzero(st.split.donor_only);
    const std::vector<size_t> idx_a = nonzero(st.split.acceptor_only);
    const std::vector<size_t> idx_f = nonzero(st.split.fret);
    st.boot_resamples += 1;
    if (!idx_d.empty()) {
        const auto s = positions(s_d, idx_d.size(), o.seed, st.boot_draws);
        st.boot_alpha.push_back(leakage_from_donor_only(take(dd, idx_d, s), take(da, idx_d, s),
                                                        f.bg_dd, f.bg_da));
    }
    if (!aa.empty() && !idx_a.empty()) {
        const auto s = positions(s_a, idx_a.size(), o.seed, st.boot_draws);
        st.boot_delta.push_back(direct_excitation_from_acceptor_only(
            take(da, idx_a, s), take(aa, idx_a, s), take(dd, idx_a, s), f.alpha, f.bg_dd, f.bg_da,
            f.bg_aa));
    }
    if (idx_f.empty()) return;
    const auto s = positions(s_f, idx_f.size(), o.seed, st.boot_draws);
    const std::vector<int> labels = take(st.split.fret_labels, idx_f, s);
    const std::set<int> uniq(labels.begin(), labels.end());
    if (!aa.empty() && uniq.size() >= 2) {
        std::vector<double> est = global_es_correction(take(dd, idx_f, s), take(da, idx_f, s),
                                                       take(aa, idx_f, s), labels, f.alpha, f.delta);
        if (std::isfinite(est[0])) st.boot_gamma.push_back(est[0]);
        if (std::isfinite(est[1])) st.boot_beta.push_back(est[1]);
    } else if (!st.data_tau.empty() && !o.line_tau_f.empty()) {
        LifetimeGamma lt = gamma_from_lifetime(
            take(dd, idx_f, s), take(da, idx_f, s), take(st.data_tau, idx_f, s), o.line_tau_f,
            o.line_efficiency, aa.empty() ? aa : take(aa, idx_f, s), f, labels, o.min_population);
        if (std::isfinite(lt.gamma)) st.boot_gamma.push_back(lt.gamma);
    }
}

std::vector<double> refine_gamma(const std::vector<double>& g, const std::vector<double>& r,
                                 const std::vector<double>& y, const std::vector<int>& labels,
                                 double alpha, double delta, double prior_mu, double prior_sigma,
                                 double data_sigma, int n_bootstrap, unsigned int seed,
                                 const std::vector<int>& indices) {
    const std::vector<double> est = global_es_correction(g, r, y, labels, alpha, delta);
    const double gamma_data = est[0];
    const bool prior = std::isfinite(prior_sigma) && prior_sigma >= 0 && std::isfinite(prior_mu);
    const double gamma_prior = prior ? prior_mu : gamma_data;
    if (!std::isfinite(gamma_data)) return {gamma_data, est[1], kNaN, gamma_prior, kNaN};
    const size_t n = labels.size();
    if (!indices.empty() && indices.size() != static_cast<size_t>(n_bootstrap) * n)
        throw std::invalid_argument("refine_gamma: indices must be (n_bootstrap, n_bursts)");
    if (!(data_sigma >= 0)) {
        std::vector<double> boot;
        unsigned long long counter = 0;
        std::vector<size_t> all(n);
        for (size_t i = 0; i < n; ++i) all[i] = i;
        for (int k = 0; k < n_bootstrap; ++k) {
            std::vector<int> given;
            if (!indices.empty()) given.assign(indices.begin() + k * n, indices.begin() + (k + 1) * n);
            const auto s = positions(given, n, seed, counter);
            try {
                const double gb = global_es_correction(take(g, all, s), take(r, all, s), take(y, all, s),
                                                       take(labels, all, s), alpha, delta)[0];
                if (std::isfinite(gb)) boot.push_back(gb);
            } catch (const std::exception&) {
                continue;
            }
        }
        data_sigma = boot.size() > 2 ? np_std(boot) : std::abs(gamma_data) * 0.1;
    }
    data_sigma = std::max(data_sigma, 1e-6);
    double post = gamma_data;
    if (prior) {
        const double w_d = 1.0 / (data_sigma * data_sigma);
        const double w_p = 1.0 / (prior_sigma * prior_sigma);
        post = (gamma_data * w_d + gamma_prior * w_p) / (w_d + w_p);
    }
    return {gamma_data, est[1], data_sigma, gamma_prior, post};
}

} // namespace tttrlib
