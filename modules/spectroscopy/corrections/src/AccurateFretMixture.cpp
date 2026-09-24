// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretPopulations.h"
#include "AccurateFretDetail.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {

const double kTiny = DBL_MIN;  // np.finfo(float).tiny
const double kLog2Pi = std::log(2.0 * M_PI);

// log p(x_i, c) + log w_c, row-major (n, k)
void log_prob(const std::vector<double>& x, const std::vector<double>& mu,
              const std::vector<double>& var, const std::vector<double>& w,
              std::vector<double>& lp) {
    const size_t n = x.size(), k = mu.size();
    lp.resize(n * k);
    for (size_t c = 0; c < k; ++c) {
        const double v = std::max(var[c], kTiny);
        const double head = 1.0 * kLog2Pi + std::log(v);
        const double lw = std::log(std::max(w[c], kTiny));
        for (size_t i = 0; i < n; ++i) {
            const double d = x[i] - mu[c];
            lp[i * k + c] = -0.5 * (head + d * d / v) + lw;
        }
    }
}

// total log-likelihood (row logsumexp, numpy-summed) and, optionally, responsibilities
double score(const std::vector<double>& lp, size_t n, size_t k, std::vector<double>* resp) {
    std::vector<double> row(n);
    if (resp) resp->resize(n * k);
    for (size_t i = 0; i < n; ++i) {
        double m = -std::numeric_limits<double>::infinity();
        for (size_t c = 0; c < k; ++c) m = std::max(m, lp[i * k + c]);
        const bool dead = !std::isfinite(m);
        const double safe = dead ? 0.0 : m;
        double total = 0.0;
        for (size_t c = 0; c < k; ++c) total += std::exp(lp[i * k + c] - safe);
        row[i] = dead ? -std::numeric_limits<double>::infinity() : std::log(total) + safe;
        if (resp) {
            // a sample no component explains gets a uniform responsibility
            double denom = 0.0;
            for (size_t c = 0; c < k; ++c) {
                double r = dead ? 1.0 : std::exp(lp[i * k + c] - safe);
                (*resp)[i * k + c] = r;
                denom += r;
            }
            if (denom <= 0.0) denom = 1.0;
            for (size_t c = 0; c < k; ++c) (*resp)[i * k + c] /= denom;
        }
    }
    return np_sum(row);
}

MixtureResult em_1d(const std::vector<double>& x, const std::vector<double>& start,
                    int n_iterations, double tolerance, double sigma_floor) {
    const size_t k = start.size(), n = x.size();
    if (k > n) throw std::invalid_argument("gaussian_mixture_1d: more components than samples");
    double spread = np_std(x);
    if (spread == 0.0) spread = 1.0;
    const double s0 = std::max(spread / static_cast<double>(std::max<size_t>(k, 1)), sigma_floor);
    std::vector<double> mu = start, var(k, s0 * s0), w(k, 1.0 / static_cast<double>(k));
    const double wsum = np_sum(w);
    for (double& v : w) v /= wsum;
    const double reg = sigma_floor * sigma_floor;

    std::vector<double> lp, resp;
    double prev = -std::numeric_limits<double>::infinity();
    int it_done = n_iterations;
    for (int it = 1; it <= n_iterations; ++it) {
        log_prob(x, mu, var, w, lp);
        const double lb = score(lp, n, k, &resp);
        std::vector<double> nk(k, 0.0), sx(k, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t c = 0; c < k; ++c) {
                nk[c] += resp[i * k + c];
                sx[c] += resp[i * k + c] * x[i];
            }
        for (size_t c = 0; c < k; ++c) {
            nk[c] = std::max(nk[c], kTiny);
            w[c] = nk[c] / static_cast<double>(n);
            mu[c] = sx[c] / nk[c];
        }
        std::vector<double> s2(k, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t c = 0; c < k; ++c) {
                // (resp * diff)**2, not resp * diff**2: chisurf's spherical update
                const double rd = resp[i * k + c] * (x[i] - mu[c]);
                s2[c] += rd * rd;
            }
        for (size_t c = 0; c < k; ++c) var[c] = std::max(s2[c] / nk[c], kTiny) + reg;
        if (lb - prev < tolerance) { it_done = it; break; }
        prev = lb;
    }

    log_prob(x, mu, var, w, lp);
    MixtureResult r;
    r.n_components = static_cast<int>(k);
    r.n_iter = it_done;
    std::vector<double> raw_resp;
    r.log_likelihood = score(lp, n, k, &raw_resp);
    r.bic = static_cast<double>(3 * k - 1) * std::log(static_cast<double>(std::max<size_t>(n, 2)))
            - 2.0 * r.log_likelihood;
    std::vector<size_t> order(k);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return mu[a] < mu[b]; });
    for (size_t c : order) {
        r.weights.push_back(w[c]);
        r.means.push_back(mu[c]);
        r.sigmas.push_back(std::max(std::sqrt(std::max(var[c], 0.0)), sigma_floor));
    }
    r.responsibilities.resize(n * k);
    r.labels.resize(n);
    for (size_t i = 0; i < n; ++i) {
        int best = 0;
        for (size_t c = 0; c < k; ++c) {
            r.responsibilities[i * k + c] = raw_resp[i * k + order[c]];
            if (r.responsibilities[i * k + c] > r.responsibilities[i * k + best]) best = static_cast<int>(c);
        }
        r.labels[i] = best;
    }
    return r;
}

} // namespace

MixtureResult gaussian_mixture_1d(const std::vector<double>& x_in, int n_components,
                                  int n_iterations, double tolerance, double sigma_floor,
                                  const std::string& init) {
    const std::vector<double> x = finite_only(x_in);
    const int k = std::max(1, n_components);
    if (x.empty()) throw std::invalid_argument("gaussian_mixture_1d needs at least one finite sample");
    std::vector<std::vector<double>> starts;
    std::vector<double> q(k);
    for (int i = 0; i < k; ++i) q[i] = (static_cast<double>(i) + 0.5) / static_cast<double>(k);
    if (init == "auto" || init == "quantile") starts.push_back(np_quantile(x, q));
    if ((init == "auto" || init == "range") && k > 1) {
        const double lo = *std::min_element(x.begin(), x.end());
        const double hi = *std::max_element(x.begin(), x.end());
        starts.push_back(hi > lo ? np_linspace(lo, hi, k) : std::vector<double>(k, lo));
    }
    if (starts.empty()) starts.push_back(np_quantile(x, q));
    MixtureResult best;
    bool have = false;
    for (const auto& s : starts) {
        MixtureResult fit = em_1d(x, s, n_iterations, tolerance, sigma_floor);
        if (!have || fit.log_likelihood > best.log_likelihood) { best = fit; have = true; }
    }
    return best;
}

MixtureResult best_gaussian_mixture_1d(const std::vector<double>& x_in, int max_components,
                                       double min_weight) {
    const std::vector<double> x = finite_only(x_in);
    MixtureResult best;
    bool have = false;
    std::vector<int> ks;
    std::vector<double> bics;
    for (int k = 1; k <= max_components; ++k) {
        if (x.size() < static_cast<size_t>(5 * k)) break;
        MixtureResult fit;
        try {
            fit = gaussian_mixture_1d(x, k);
        } catch (const std::invalid_argument&) {
            break;
        }
        ks.push_back(k);
        bics.push_back(fit.bic);
        if (k > 1 && *std::min_element(fit.weights.begin(), fit.weights.end()) < min_weight) continue;
        if (!have || fit.bic < best.bic) { best = fit; have = true; }
    }
    if (!have) best = gaussian_mixture_1d(x, 1);
    best.bic_k = ks;
    best.bic_values = bics;
    return best;
}

} // namespace tttrlib
