// PoissonScore.h: the Poisson count model's deviance, residuals, gradient and
// information matrix.
//
// Checks: the gradient is the derivative of log L = sum w (y log m - m) (central
// differences of a log-linear model); the expected information is J' diag(w/m) J
// and the observed J' diag(w y/m^2) J (written out here); the blocked form equals
// the full one when each row block depends on its own columns; the deviance
// residuals square-sum to the deviance; and fit2x's twoIstar equals the deviance
// over 2N when the model's total equals the data's -- and does not when it is
// scaled (the control).
//
//     c++ -std=c++17 -O2 -I modules/math/include -I modules/spectroscopy/decay/include \
//         -I modules/util/include -o t test/cpp/test_poisson_score.cpp \
//         modules/spectroscopy/decay/src/DecayStatistics.cpp modules/util/src/Verbose.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "PoissonScore.h"
#include "DecayStatistics.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-66s %.3e\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

int main() {
    std::mt19937_64 rng(3);
    std::normal_distribution<double> N(0.0, 1.0);
    const std::size_t nb = 60, np = 5;
    std::vector<double> X(nb * np), beta(np), w(nb), y(nb);
    for (double &v : X) v = 0.3 * N(rng);
    for (double &v : beta) v = 0.5 * N(rng);
    for (std::size_t b = 0; b < nb; ++b) w[b] = (b % 7 == 3) ? 0.0 : 1.0;
    auto mean = [&](const std::vector<double> &be, std::vector<double> &m, std::vector<double> *J) {
        m.assign(nb, 0.0);
        if (J) J->assign(nb * np, 0.0);
        for (std::size_t b = 0; b < nb; ++b) {
            double e = 3.0;
            for (std::size_t p = 0; p < np; ++p) e += X[b * np + p] * be[p];
            m[b] = std::exp(e);
            if (J) for (std::size_t p = 0; p < np; ++p) (*J)[b * np + p] = m[b] * X[b * np + p];
        }
    };
    std::vector<double> m, J;
    mean(beta, m, &J);
    for (std::size_t b = 0; b < nb; ++b) y[b] = double(std::poisson_distribution<int>(m[b])(rng));
    auto logL = [&](const std::vector<double> &be) {
        std::vector<double> mm; mean(be, mm, nullptr);
        double s = 0.0;
        for (std::size_t b = 0; b < nb; ++b) s += w[b] * (y[b] * std::log(mm[b]) - mm[b]);
        return s;
    };
    std::vector<double> g(np), A(np * np), Ao(np * np);
    tttrlib::poisson_score(y.data(), m.data(), J.data(), w.data(), nb, np, tttrlib::POISSON_INFORMATION_EXPECTED, g.data(), A.data());
    tttrlib::poisson_score(y.data(), m.data(), J.data(), w.data(), nb, np, tttrlib::POISSON_INFORMATION_OBSERVED, g.data(), Ao.data());
    double dg = 0.0, gs = 0.0;
    for (std::size_t p = 0; p < np; ++p) {
        std::vector<double> bp = beta, bm = beta;
        bp[p] += 1e-6; bm[p] -= 1e-6;
        const double fd = (logL(bp) - logL(bm)) / 2e-6;
        dg = std::max(dg, std::fabs(fd - g[p])); gs = std::max(gs, std::fabs(fd));
    }
    check(dg / gs < 1e-6, "gradient = d log L (central differences)", dg / gs);
    double dA = 0.0, dAo = 0.0, As = 0.0;
    for (std::size_t p = 0; p < np; ++p) for (std::size_t q = 0; q < np; ++q) {
        double e = 0.0, o = 0.0;
        for (std::size_t b = 0; b < nb; ++b) {
            e += w[b] / m[b] * J[b * np + p] * J[b * np + q];
            o += w[b] * y[b] / (m[b] * m[b]) * J[b * np + p] * J[b * np + q];
        }
        dA = std::max(dA, std::fabs(e - A[p * np + q])); dAo = std::max(dAo, std::fabs(o - Ao[p * np + q]));
        As = std::max(As, std::fabs(e));
    }
    check(dA / As < 1e-12 && dAo / As < 1e-12, "expected and observed information written out", std::max(dA, dAo) / As);
    // blocks: rows [0,30) see columns {0,1,2}, rows [30,60) columns {2,3,4}
    std::vector<double> Jb(J);
    for (std::size_t b = 0; b < nb; ++b) for (std::size_t p = 0; p < np; ++p)
        if ((b < 30 && p > 2) || (b >= 30 && p < 2)) Jb[b * np + p] = 0.0;
    std::vector<double> g1(np), A1(np * np), g2(np), A2(np * np);
    tttrlib::poisson_score(y.data(), m.data(), Jb.data(), w.data(), nb, np, tttrlib::POISSON_INFORMATION_EXPECTED, g1.data(), A1.data());
    auto serial = [](std::size_t n, auto body) { for (std::size_t k = 0; k < n; ++k) body(k); };
    tttrlib::poisson_score_blocks(y.data(), m.data(), Jb.data(), w.data(), nb, np, tttrlib::POISSON_INFORMATION_EXPECTED,
                                  {{0, 1, 2}, {2, 3, 4}}, 30, g2.data(), A2.data(), serial);
    double db = 0.0;
    for (std::size_t i = 0; i < np; ++i) db = std::max(db, std::fabs(g1[i] - g2[i]) / (std::fabs(g1[i]) + 1e-300));
    for (std::size_t i = 0; i < np * np; ++i) db = std::max(db, std::fabs(A1[i] - A2[i]) / As);
    check(db < 1e-12, "blocked score = full score on block-sparse rows", db);
    // deviance and its residuals
    std::vector<double> r(nb);
    tttrlib::deviance_residuals(y.data(), m.data(), nb, r.data());
    double ss = 0.0;
    for (double v : r) ss += v * v;
    const double D = tttrlib::poisson_deviance(y.data(), m.data(), nb);
    check(std::fabs(ss - D) / D < 1e-12, "deviance residuals square-sum to the deviance", std::fabs(ss - D) / D);
    // fit2x's 2I*: channels in pairs (2N), model scaled to the data's total
    const int Nch = int(nb / 2);
    std::vector<int> C(nb);
    double sy = 0.0, sm = 0.0;
    for (std::size_t b = 0; b < nb; ++b) { C[b] = int(y[b]); sy += y[b]; sm += m[b]; }
    std::vector<double> M(nb), Ms(nb);
    for (std::size_t b = 0; b < nb; ++b) { M[b] = m[b] * sy / sm; Ms[b] = 1.3 * M[b]; }
    const double I2 = twoIstar(C.data(), M.data(), Nch), Dn = tttrlib::poisson_deviance(y.data(), M.data(), nb);
    check(std::fabs(I2 - Dn / (2.0 * Nch)) / I2 < 1e-12, "twoIstar = deviance / 2N when totals match", std::fabs(I2 - Dn / (2.0 * Nch)) / I2);
    const double I2s = twoIstar(C.data(), Ms.data(), Nch), Ds = tttrlib::poisson_deviance(y.data(), Ms.data(), nb);
    check(std::fabs(I2s - Ds / (2.0 * Nch)) / std::fabs(Ds / (2.0 * Nch)) > 1e-2, "control: not when the model's total is scaled", std::fabs(I2s - Ds / (2.0 * Nch)) / std::fabs(Ds / (2.0 * Nch)));
    return failures;
}
