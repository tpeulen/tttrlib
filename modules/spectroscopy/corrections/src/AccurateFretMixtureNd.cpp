// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretMultiDim.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace tttrlib {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kLog2Pi = std::log(2.0 * M_PI);

struct Fit {
    std::vector<double> w, mu, var, resp;  // standardised units
    double ll = 0.0;
    int n_iter = 0;
};

// E-step on standardised rows z (n x d): responsibilities and total log-likelihood
double e_step(const std::vector<double>& z, size_t n, size_t d, const Fit& f, std::vector<double>& resp) {
    const size_t k = f.w.size();
    resp.assign(n * k, 0.0);
    double total = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double m = -std::numeric_limits<double>::infinity();
        for (size_t c = 0; c < k; ++c) {
            // a missing value is marginalised: the diagonal density factorises
            double q = -2.0 * std::log(std::max(f.w[c], 1e-300));
            for (size_t j = 0; j < d; ++j) {
                const double v = z[i * d + j];
                if (!std::isfinite(v)) continue;
                const double t = v - f.mu[c * d + j];
                q += kLog2Pi + std::log(f.var[c * d + j]) + t * t / f.var[c * d + j];
            }
            resp[i * k + c] = -0.5 * q;
            m = std::max(m, resp[i * k + c]);
        }
        double s = 0.0;
        for (size_t c = 0; c < k; ++c) s += std::exp(resp[i * k + c] - m);
        for (size_t c = 0; c < k; ++c) resp[i * k + c] = std::exp(resp[i * k + c] - m) / s;
        total += std::log(s) + m;
    }
    return total;
}

Fit start(const std::vector<double>& z, size_t n, size_t d, size_t k, size_t along, double floor2) {
    // rows ordered along one dimension, cut into k equal groups; rows missing that
    // dimension go last, their other values still seed the groups
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const bool fa = std::isfinite(z[a * d + along]), fb = std::isfinite(z[b * d + along]);
        return fa != fb ? fa : (fa && z[a * d + along] < z[b * d + along]);
    });
    Fit f;
    f.w.assign(k, 0.0);
    f.mu.assign(k * d, 0.0);
    f.var.assign(k * d, 0.0);
    std::vector<double> cnt(k, 0.0), cj(k * d, 0.0);
    for (size_t r = 0; r < n; ++r) {
        const size_t c = std::min(k - 1, r * k / n), i = order[r];
        cnt[c] += 1;
        for (size_t j = 0; j < d; ++j)
            if (std::isfinite(z[i * d + j])) { f.mu[c * d + j] += z[i * d + j]; cj[c * d + j] += 1; }
    }
    for (size_t c = 0; c < k * d; ++c) f.mu[c] = cj[c] > 0 ? f.mu[c] / cj[c] : 0.0;
    for (size_t r = 0; r < n; ++r) {
        const size_t c = std::min(k - 1, r * k / n), i = order[r];
        for (size_t j = 0; j < d; ++j) {
            if (!std::isfinite(z[i * d + j])) continue;
            const double t = z[i * d + j] - f.mu[c * d + j];
            f.var[c * d + j] += t * t;
        }
    }
    for (size_t c = 0; c < k; ++c) {
        f.w[c] = cnt[c] / static_cast<double>(n);
        for (size_t j = 0; j < d; ++j)
            f.var[c * d + j] = (cj[c * d + j] > 0 ? f.var[c * d + j] / cj[c * d + j] : 1.0) + floor2;
    }
    return f;
}

void em(Fit& f, const std::vector<double>& z, size_t n, size_t d, size_t k, int n_iterations,
        double tolerance, double floor2) {
    double prev = -std::numeric_limits<double>::infinity();
    f.n_iter = n_iterations;
    for (int it = 1; it <= n_iterations; ++it) {
        const double ll = e_step(z, n, d, f, f.resp);
        std::vector<double> nk(k, 0.0), nkj(k * d, 0.0), sx(k * d, 0.0), sxx(k * d, 0.0);
        for (size_t i = 0; i < n; ++i)
            for (size_t c = 0; c < k; ++c) {
                const double r = f.resp[i * k + c];
                nk[c] += r;
                for (size_t j = 0; j < d; ++j) {
                    const double v = z[i * d + j];
                    if (!std::isfinite(v)) continue;
                    nkj[c * d + j] += r;
                    sx[c * d + j] += r * v;
                }
            }
        for (size_t c = 0; c < k; ++c) {
            f.w[c] = std::max(nk[c], 1e-300) / static_cast<double>(n);
            for (size_t j = 0; j < d; ++j)
                if (nkj[c * d + j] > 1e-300) f.mu[c * d + j] = sx[c * d + j] / nkj[c * d + j];
        }
        for (size_t i = 0; i < n; ++i)
            for (size_t c = 0; c < k; ++c)
                for (size_t j = 0; j < d; ++j) {
                    const double v = z[i * d + j];
                    if (!std::isfinite(v)) continue;
                    const double t = v - f.mu[c * d + j];
                    sxx[c * d + j] += f.resp[i * k + c] * t * t;
                }
        for (size_t c = 0; c < k * d; ++c)
            if (nkj[c] > 1e-300) f.var[c] = sxx[c] / nkj[c] + floor2;
        // per-sample gain: the total grows with n, the stopping rule should not
        if ((ll - prev) / static_cast<double>(n) < tolerance) { f.n_iter = it; break; }
        prev = ll;
    }
    f.ll = e_step(z, n, d, f, f.resp);
}

} // namespace

MixtureNdResult gaussian_mixture_nd(const std::vector<double>& x, int n_rows, int n_dims,
                                    int n_components, int n_iterations, double tolerance,
                                    double sigma_floor) {
    if (n_rows < 0 || n_dims <= 0 || x.size() != static_cast<size_t>(n_rows) * n_dims)
        throw std::invalid_argument("gaussian_mixture_nd: x must be (n_rows, n_dims)");
    const size_t d = n_dims, k = std::max(1, n_components);
    std::vector<size_t> rows;
    for (size_t i = 0; i < static_cast<size_t>(n_rows); ++i) {
        bool any = false;
        for (size_t j = 0; j < d; ++j) any = any || std::isfinite(x[i * d + j]);
        if (any) rows.push_back(i);
    }
    const size_t n = rows.size();
    if (n < k) throw std::invalid_argument("gaussian_mixture_nd: fewer usable rows than components");
    std::vector<double> mean(d, 0.0), sd(d, 0.0), cnt(d, 0.0), z(n * d);
    for (size_t j = 0; j < d; ++j) {
        for (size_t r : rows)
            if (std::isfinite(x[r * d + j])) { mean[j] += x[r * d + j]; cnt[j] += 1; }
        mean[j] = cnt[j] > 0 ? mean[j] / cnt[j] : 0.0;
        for (size_t r : rows)
            if (std::isfinite(x[r * d + j])) sd[j] += (x[r * d + j] - mean[j]) * (x[r * d + j] - mean[j]);
        sd[j] = cnt[j] > 0 ? std::sqrt(sd[j] / cnt[j]) : 1.0;
        if (!(sd[j] > 0)) sd[j] = 1.0;
    }
    for (size_t r = 0; r < n; ++r)
        for (size_t j = 0; j < d; ++j) z[r * d + j] = (x[rows[r] * d + j] - mean[j]) / sd[j];
    // one start per dimension (species can differ in any one of them), a short
    // run each, and the most likely one taken to convergence
    const double floor2 = sigma_floor * sigma_floor;
    Fit f;
    for (size_t along = 0; along < d; ++along) {
        Fit g = start(z, n, d, k, along, floor2);
        em(g, z, n, d, k, std::min(n_iterations, 20), tolerance, floor2);
        if (along == 0 || g.ll > f.ll) f = g;
    }
    em(f, z, n, d, k, n_iterations, tolerance, floor2);

    MixtureNdResult out;
    out.n_components = static_cast<int>(k);
    out.n_dims = static_cast<int>(d);
    out.n_iter = f.n_iter;
    double log_sd = 0.0;
    for (size_t j = 0; j < d; ++j) log_sd += cnt[j] * std::log(sd[j]);
    out.log_likelihood = f.ll - log_sd;  // back to input units
    const double n_free = static_cast<double>(2 * k * d + k - 1);
    out.bic = n_free * std::log(static_cast<double>(std::max<size_t>(n, 2))) - 2.0 * out.log_likelihood;
    std::vector<size_t> order(k);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return f.mu[a * d] < f.mu[b * d]; });
    for (size_t c : order) {
        out.weights.push_back(f.w[c]);
        for (size_t j = 0; j < d; ++j) {
            out.means.push_back(f.mu[c * d + j] * sd[j] + mean[j]);
            out.sigmas.push_back(std::sqrt(f.var[c * d + j]) * sd[j]);
        }
    }
    out.responsibilities.assign(static_cast<size_t>(n_rows) * k, kNaN);
    out.labels.assign(n_rows, -1);
    for (size_t r = 0; r < n; ++r) {
        int best = 0;
        for (size_t c = 0; c < k; ++c) {
            const double v = f.resp[r * k + order[c]];
            out.responsibilities[rows[r] * k + c] = v;
            if (v > out.responsibilities[rows[r] * k + best]) best = static_cast<int>(c);
        }
        out.labels[rows[r]] = best;
    }
    return out;
}

MixtureNdResult best_gaussian_mixture_nd(const std::vector<double>& x, int n_rows, int n_dims,
                                         int max_components, double min_weight) {
    size_t n_finite = 0;
    for (int i = 0; i < n_rows; ++i) {
        bool any = false;
        for (int j = 0; j < n_dims; ++j) any = any || std::isfinite(x[static_cast<size_t>(i) * n_dims + j]);
        n_finite += any;
    }
    MixtureNdResult best;
    bool have = false;
    std::vector<int> ks;
    std::vector<double> bics;
    for (int k = 1; k <= max_components; ++k) {
        if (n_finite < static_cast<size_t>(5 * k)) break;
        MixtureNdResult fit = gaussian_mixture_nd(x, n_rows, n_dims, k);
        ks.push_back(k);
        bics.push_back(fit.bic);
        if (k > 1 && *std::min_element(fit.weights.begin(), fit.weights.end()) < min_weight) continue;
        if (!have || fit.bic < best.bic) { best = fit; have = true; }
    }
    if (!have) best = gaussian_mixture_nd(x, n_rows, n_dims, 1);
    best.bic_k = ks;
    best.bic_values = bics;
    return best;
}

} // namespace tttrlib
