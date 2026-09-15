// NoUTurnSampler.h, against targets whose answers are known.
//
// Checks, each of which can fail:
// - a 20-d correlated Gaussian (condition 1e4), 4 chains with adapted step size and identity metric:
//   every mean and variance within 4 Monte Carlo standard errors of the truth, rank R-hat < 1.01 and
//   bulk ESS > 400 on every coordinate (McmcDiagnostics.h);
// - with the true covariance as the inverse metric the tree depth falls and the ESS per gradient
//   evaluation rises (the metric is used);
// - a maximum tree depth of 1 (two leapfrog steps, no U-turn freedom) gives a far smaller ESS per
//   draw on the ill-conditioned target (the control);
// - Neal's funnel with a fixed, too large step size reports divergences.
//
//     c++ -std=c++17 -O2 -I modules/math/include -o t test/cpp/test_no_u_turn_sampler.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "McmcDiagnostics.h"
#include "NoUTurnSampler.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-74s %.6g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

struct Run { tttrlib::McmcChains x; double mean_depth = 0, grads = 0, divergences = 0; };

static Run sample(std::size_t n, const tttrlib::NoUTurnSampler::Density& f, const std::vector<double>* metric, int chains, int warm,
                  int draws, int max_depth, std::uint64_t seed, double fixed_eps = 0.0) {
    Run r;
    r.x.assign(chains * n, std::vector<double>(draws));
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> N(0.0, 1.0);
    for (int c = 0; c < chains; ++c) {
        tttrlib::NoUTurnSampler s(n, f, seed + 101 * c);
        if (metric) s.set_inverse_metric(*metric);
        s.set_max_depth(max_depth);
        std::vector<double> q0(n);
        for (double& v : q0) v = 0.5 * N(rng);
        s.set_state(q0);
        if (fixed_eps > 0.0) {
            s.set_step_size(fixed_eps);
        } else {
            s.init_step_size();
            s.begin_adaptation();
            for (int k = 0; k < warm; ++k) s.transition();
            s.end_adaptation();
        }
        for (int k = 0; k < draws; ++k) {
            const auto t = s.transition();
            for (std::size_t i = 0; i < n; ++i) r.x[c * n + i][k] = t.q[i];
            r.mean_depth += t.tree_depth; r.grads += t.n_leapfrog; r.divergences += t.divergent;
        }
    }
    r.mean_depth /= chains * draws;
    return r;
}

static tttrlib::McmcChains coord(const Run& r, std::size_t n, int chains, std::size_t i) {
    tttrlib::McmcChains c(chains);
    for (int a = 0; a < chains; ++a) c[a] = r.x[a * n + i];
    return c;
}

int main() {
    const std::size_t n = 20;
    // covariance: eigenvalues 1e-4 .. 1 (geometric) in a random orthonormal basis
    std::mt19937_64 rng(5);
    std::normal_distribution<double> N(0.0, 1.0);
    std::vector<double> Q(n * n);
    for (double& v : Q) v = N(rng);
    for (std::size_t c = 0; c < n; ++c) {
        for (std::size_t p = 0; p < c; ++p) {
            double d = 0; for (std::size_t r = 0; r < n; ++r) d += Q[r * n + c] * Q[r * n + p];
            for (std::size_t r = 0; r < n; ++r) Q[r * n + c] -= d * Q[r * n + p];
        }
        double nn = 0; for (std::size_t r = 0; r < n; ++r) nn += Q[r * n + c] * Q[r * n + c];
        nn = std::sqrt(nn); for (std::size_t r = 0; r < n; ++r) Q[r * n + c] /= nn;
    }
    std::vector<double> lam(n), C(n * n, 0.0), P(n * n, 0.0), mu(n);
    for (std::size_t k = 0; k < n; ++k) lam[k] = std::pow(10.0, -4.0 * double(k) / double(n - 1));
    for (std::size_t i = 0; i < n; ++i) { mu[i] = 0.3 * N(rng); for (std::size_t j = 0; j < n; ++j) for (std::size_t k = 0; k < n; ++k) {
        C[i * n + j] += Q[i * n + k] * lam[k] * Q[j * n + k]; P[i * n + j] += Q[i * n + k] / lam[k] * Q[j * n + k]; } }
    const tttrlib::NoUTurnSampler::Density gauss = [&](const std::vector<double>& x, std::vector<double>& g) {
        g.assign(n, 0.0); double lp = 0;
        for (std::size_t i = 0; i < n; ++i) { double s = 0; for (std::size_t j = 0; j < n; ++j) s += P[i * n + j] * (x[j] - mu[j]); g[i] = -s; lp -= 0.5 * (x[i] - mu[i]) * s; }
        return lp;
    };
    const int chains = 4, warm = 1000, draws = 1000;
    const Run id = sample(n, gauss, nullptr, chains, warm, draws, 10, 11);
    double worst_mean = 0, worst_var = 0, worst_rhat = 0, min_ess = 1e300;
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = coord(id, n, chains, i);
        double m = 0; for (auto& ch : c) for (double v : ch) m += v; m /= chains * draws;
        worst_mean = std::max(worst_mean, std::fabs(m - mu[i]) / tttrlib::mcse_mean(c));
        tttrlib::McmcChains sq = c;
        for (auto& ch : sq) for (double& v : ch) v = (v - mu[i]) * (v - mu[i]);
        double vbar = 0; for (auto& ch : sq) for (double v : ch) vbar += v; vbar /= chains * draws;
        worst_var = std::max(worst_var, std::fabs(vbar - C[i * n + i]) / tttrlib::mcse_mean(sq));
        worst_rhat = std::max(worst_rhat, tttrlib::rhat_rank(c));
        min_ess = std::min(min_ess, tttrlib::ess_bulk(c));
    }
    check(worst_mean < 4.0, "Gaussian, identity metric: every mean within 4 MCSE (worst, in MCSE)", worst_mean);
    check(worst_var < 4.0, "Gaussian, identity metric: every variance within 4 MCSE (worst, in MCSE)", worst_var);
    check(worst_rhat < 1.01, "Gaussian, identity metric: rank R-hat < 1.01 (worst)", worst_rhat);
    check(min_ess > 400.0, "Gaussian, identity metric: bulk ESS > 400 (smallest)", min_ess);

    const Run dense = sample(n, gauss, &C, chains, 300, draws, 10, 12);
    double ess_id = 1e300, ess_dense = 1e300;
    for (std::size_t i = 0; i < n; ++i) { ess_id = std::min(ess_id, tttrlib::ess_bulk(coord(id, n, chains, i))); ess_dense = std::min(ess_dense, tttrlib::ess_bulk(coord(dense, n, chains, i))); }
    check(dense.mean_depth < id.mean_depth - 1.0, "true covariance as metric: mean tree depth drops by > 1", id.mean_depth - dense.mean_depth);
    check(ess_dense / dense.grads > 5.0 * ess_id / id.grads, "true covariance as metric: ESS per gradient up > 5x", (ess_dense / dense.grads) / (ess_id / id.grads));

    const Run shallow = sample(n, gauss, nullptr, chains, warm, draws, 1, 13);
    double ess_shallow = 1e300;
    for (std::size_t i = 0; i < n; ++i) ess_shallow = std::min(ess_shallow, tttrlib::ess_bulk(coord(shallow, n, chains, i)));
    check(ess_shallow < 0.2 * min_ess, "control: max tree depth 1 gives a far smaller ESS per draw", ess_shallow / min_ess);

    // Neal's funnel, 10-d: v ~ N(0, 3^2), x_k | v ~ N(0, e^v)
    const std::size_t nf = 10;
    const tttrlib::NoUTurnSampler::Density funnel = [&](const std::vector<double>& x, std::vector<double>& g) {
        g.assign(nf, 0.0);
        const double v = x[0];
        double lp = -v * v / 18.0; g[0] = -v / 9.0;
        for (std::size_t k = 1; k < nf; ++k) { lp += -0.5 * x[k] * x[k] * std::exp(-v) - 0.5 * v; g[0] += 0.5 * x[k] * x[k] * std::exp(-v) - 0.5; g[k] = -x[k] * std::exp(-v); }
        return lp;
    };
    const Run fun = sample(nf, funnel, nullptr, 2, 0, 500, 10, 14, 0.8);
    check(fun.divergences > 0, "Neal's funnel with a fixed step 0.8: divergences reported", fun.divergences);
    return failures;
}
