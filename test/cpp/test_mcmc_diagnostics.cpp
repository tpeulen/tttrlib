// McmcDiagnostics.h: rank-normalised split R-hat, ESS (bulk, tail, mean) and MCSE.
//
// Checks: the normal quantile function (AS241) at tabulated values; on AR(1) chains with known
// rho the ESS of the mean is within 10 % of N (1 - rho)/(1 + rho) and the bulk ESS is close to it;
// independent chains give R-hat within 1.01 of one; chains whose means differ by half their sd give
// R-hat > 1.1, and by a tenth the value the variance ratio predicts (the control -- if they passed, the diagnostic could not see non-mixing);
// chains that agree in mean but differ in scale are caught by the TAIL part of R-hat; MCSE of the
// mean is sd/sqrt(ESS). The agreement with arviz itself is imp.bff's test_mcmc_diagnostics.py.
//
//     c++ -std=c++17 -O2 -I modules/math/include -o t test/cpp/test_mcmc_diagnostics.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "McmcDiagnostics.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-70s %.6g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

static tttrlib::McmcChains ar1(std::size_t m, std::size_t n, double rho, std::uint64_t seed, double shift_step = 0.0, double scale_step = 0.0) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> N(0.0, 1.0);
    tttrlib::McmcChains x(m, std::vector<double>(n));
    const double s = std::sqrt(1.0 - rho * rho);
    for (std::size_t a = 0; a < m; ++a) {
        double v = N(rng);
        for (std::size_t i = 0; i < n; ++i) {
            v = rho * v + s * N(rng);
            x[a][i] = double(a) * shift_step + (1.0 + double(a) * scale_step) * v;
        }
    }
    return x;
}

int main() {
    using namespace tttrlib;
    double q = std::max({std::fabs(normal_quantile(0.975) - 1.959963984540054), std::fabs(normal_quantile(0.25) + 0.6744897501960817),
                         std::fabs(normal_quantile(1e-10) + 6.361340902404056), std::fabs(normal_quantile(0.5))});
    check(q < 1e-14, "normal quantile (AS241) at tabulated values", q);

    const std::size_t m = 4, n = 4000, N = m * n;
    const auto iid = ar1(m, n, 0.0, 1);
    check(std::fabs(rhat_rank(iid) - 1.0) < 0.01, "independent chains: rank R-hat within 0.01 of 1", rhat_rank(iid));
    check(ess_bulk(iid) > 0.8 * N, "independent chains: bulk ESS near N", ess_bulk(iid) / N);

    for (double rho : {0.5, 0.9}) {
        // one run's ESS estimate scatters by several per cent at these lengths: average over seeds
        const double expect = N * (1.0 - rho) / (1.0 + rho);
        double em = 0.0, eb = 0.0;
        const int seeds = 20;
        for (int sd_ = 0; sd_ < seeds; ++sd_) { const auto xs = ar1(m, n, rho, 100 + sd_); em += ess_mean(xs) / seeds; eb += ess_bulk(xs) / seeds; }
        const auto x = ar1(m, n, rho, 2);
        char buf[96];
        std::snprintf(buf, sizeof buf, "AR(1) rho %.1f, 20 seeds: mean ESS of the mean / N(1-rho)/(1+rho)", rho);
        check(std::fabs(em / expect - 1.0) < 0.10, buf, em / expect);
        std::snprintf(buf, sizeof buf, "AR(1) rho %.1f, 20 seeds: mean bulk ESS / N(1-rho)/(1+rho)", rho);
        check(std::fabs(eb / expect - 1.0) < 0.10, buf, eb / expect);
        std::snprintf(buf, sizeof buf, "AR(1) rho %.1f: tail ESS below N", rho);
        check(ess_tail(x) < N && ess_tail(x) > 0.0, buf, ess_tail(x) / N);
    }
    // chain means 0, 0.5, 1, 1.5 sd: between-chain variance 0.42 of the within, R-hat ~ sqrt(1.3) ~ 1.14
    const auto shifted = ar1(m, n, 0.5, 3, 0.5);
    check(rhat_rank(shifted) > 1.1, "control: chain means 0.5 sd apart give R-hat > 1.1", rhat_rank(shifted));
    // against theory at a small shift: split chains' means 0, 0, 0.1, 0.1, 0.2, 0.2, 0.3, 0.3 sd have variance
    // 0.01429 (ddof 1), so R-hat ~ sqrt(1 + 0.01429) = 1.0071 (up to the within-chain noise)
    const auto slight = ar1(m, n, 0.5, 6, 0.1);
    check(std::fabs(rhat_rank(slight) - std::sqrt(1.0 + 0.0142857)) < 0.003, "chain means 0.1 sd apart: R-hat as the variance ratio predicts", rhat_rank(slight));
    const auto scaled = ar1(m, n, 0.0, 4, 0.0, 0.6);
    check(rhat_rank(scaled) > 1.01, "control: equal means, unequal scales are caught (tail R-hat)", rhat_rank(scaled));
    const auto x = ar1(m, n, 0.7, 5);
    double mean = 0, s2 = 0;
    for (auto& c : x) for (double v : c) mean += v;
    mean /= N;
    for (auto& c : x) for (double v : c) s2 += (v - mean) * (v - mean);
    const double sd = std::sqrt(s2 / (N - 1));
    check(std::fabs(mcse_mean(x) - sd / std::sqrt(ess_mean(x))) < 1e-15, "MCSE of the mean = sd / sqrt(ESS)", mcse_mean(x));
    return failures;
}
