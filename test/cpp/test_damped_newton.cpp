// DampedNewton.h: the Cholesky pieces against Gaussian elimination, and the stepper
// on a Poisson regression.
//
// Checks: cholesky_solve against a partial-pivoting Gaussian elimination written
// here; log_det_spd against the log of that elimination's pivots; a matrix that is
// not positive definite is refused; DampedNewton with Fisher scoring drives a
// Poisson log-linear regression to a vanishing gradient; line_search_below switches
// backtracking on only once the last decrement is below it, and its default (infinity)
// backtracks from the first step -- which it did not until 2026-09-15; and a gradient of the
// wrong sign is never accepted (the stepper reports failure, not a step). RidgeProjector (added
// 2026-09-15) against the same elimination on the ridged normal equations, with a collinear pair,
// the scale invariance of a relative lambda, and an absolute one as its negative control.
//
//     c++ -std=c++17 -O2 -I modules/math/include -o t test/cpp/test_damped_newton.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "DampedNewton.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-66s %.3e\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

// Gaussian elimination with partial pivoting: solves A x = b, returns log|det A|
static double gauss(std::vector<double> A, std::vector<double> b, std::size_t n, std::vector<double> &x) {
    double logdet = 0.0;
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r) if (std::fabs(A[r * n + c]) > std::fabs(A[piv * n + c])) piv = r;
        if (piv != c) { for (std::size_t k = 0; k < n; ++k) std::swap(A[c * n + k], A[piv * n + k]); std::swap(b[c], b[piv]); }
        logdet += std::log(std::fabs(A[c * n + c]));
        for (std::size_t r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            for (std::size_t k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    x.assign(n, 0.0);
    for (std::size_t r = n; r-- > 0;) {
        double s = b[r];
        for (std::size_t k = r + 1; k < n; ++k) s -= A[r * n + k] * x[k];
        x[r] = s / A[r * n + r];
    }
    return logdet;
}

int main() {
    std::mt19937_64 rng(11);
    std::normal_distribution<double> N(0.0, 1.0);
    // an SPD matrix spanning a few decades
    const std::size_t n = 30;
    std::vector<double> G(n * n), A(n * n, 0.0), b(n);
    for (double &g : G) g = N(rng);
    for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) {
        double s = 0.0; for (std::size_t k = 0; k < n; ++k) s += G[i * n + k] * G[j * n + k];
        A[i * n + j] = s + (i == j ? std::pow(10.0, double(i) / 10.0) : 0.0);
    }
    for (double &x : b) x = N(rng);
    std::vector<double> xc(n), xg;
    const bool ok = tttrlib::cholesky_solve(A.data(), b.data(), n, xc.data());
    const double ld_g = gauss(A, b, n, xg);
    double dx = 0.0, xs = 0.0;
    for (std::size_t i = 0; i < n; ++i) { dx = std::max(dx, std::fabs(xc[i] - xg[i])); xs = std::max(xs, std::fabs(xg[i])); }
    check(ok && dx / xs < 1e-10, "cholesky_solve against Gaussian elimination (relative)", dx / xs);
    double ld = 0.0;
    const bool okd = tttrlib::log_det_spd(A.data(), n, &ld);
    check(okd && std::fabs(ld - ld_g) < 1e-10, "log_det_spd against the elimination's pivots", std::fabs(ld - ld_g));
    std::vector<double> B = A; B[5 * n + 5] = -1e6;
    double dummy;
    check(!tttrlib::log_det_spd(B.data(), n, &dummy) && !tttrlib::cholesky_solve(B.data(), b.data(), n, xc.data()),
          "a matrix that is not positive definite is refused", 0.0);

    // Poisson log-linear regression: y ~ Poisson(exp(X beta)), minimise -log L
    const std::size_t m = 400, p = 4;
    std::vector<double> X(m * p), y(m), beta_true = {0.5, -0.3, 0.8, 1.2};
    for (std::size_t i = 0; i < m; ++i) {
        X[i * p] = 1.0; for (std::size_t j = 1; j < p; ++j) X[i * p + j] = 0.5 * N(rng);
        double eta = 0.0; for (std::size_t j = 0; j < p; ++j) eta += X[i * p + j] * beta_true[j];
        y[i] = double(std::poisson_distribution<long long>(std::exp(eta))(rng));
    }
    auto f = [&](const double *beta) {
        double s = 0.0;
        for (std::size_t i = 0; i < m; ++i) { double eta = 0.0; for (std::size_t j = 0; j < p; ++j) eta += X[i * p + j] * beta[j]; s += std::exp(eta) - y[i] * eta; }
        return s;
    };
    auto score = [&](const std::vector<double> &beta, std::vector<double> &g, std::vector<double> &F) {
        g.assign(p, 0.0); F.assign(p * p, 0.0);
        for (std::size_t i = 0; i < m; ++i) {
            double eta = 0.0; for (std::size_t j = 0; j < p; ++j) eta += X[i * p + j] * beta[j];
            const double mu = std::exp(eta);
            for (std::size_t a = 0; a < p; ++a) { g[a] += (y[i] - mu) * X[i * p + a]; for (std::size_t c = 0; c < p; ++c) F[a * p + c] += mu * X[i * p + a] * X[i * p + c]; }
        }
    };
    tttrlib::DampedNewton<decltype(f)> stepper;
    std::vector<double> beta(p, 0.0), g, F;
    int steps = 0;
    for (; steps < 100; ++steps) {
        score(beta, g, F);
        const auto r = stepper.step(F.data(), g.data(), p, f(beta.data()), beta.data(), f);
        if (!r.accepted || r.decrement < 1e-12) break;
    }
    score(beta, g, F);
    double gn = 0.0; for (double x : g) gn = std::max(gn, std::fabs(x));
    check(gn < 1e-6, "Fisher scoring reaches a vanishing gradient on a Poisson regression", gn);

    // line_search_below: the full step overshoots on a steep exponential; backtracking only below the threshold
    auto h = [&](const double *x) { return std::exp(3.0 * x[0]) - 3.0 * x[0]; };
    const double x0 = 1.5, g1 = -(3.0 * std::exp(3.0 * x0) - 3.0), A1 = 9.0 * std::exp(3.0 * x0) * 0.005;   // curvature underestimated 200x: the full step lands uphill
    tttrlib::DampedNewton<decltype(h)> s_on, s_off;
    s_on.mu = 0.0; s_on.mu_min = 0.0; s_off.mu = 1e-3;   // off: re-damping needs a damping to grow
    s_off.line_search_below = -1.0;                  // never: the last decrement is never below -1
    double xa = x0, xb = x0;
    const auto ra = s_on.step(&A1, &g1, 1, h(&xa), &xa, h);
    const auto rb = s_off.step(&A1, &g1, 1, h(&xb), &xb, h);
    check(ra.accepted && ra.alpha < 1.0 && rb.mu > 0.0, "line_search_below: backtracks when on, re-damps when off", ra.alpha);

    // negative control: a gradient pointing uphill is not accepted
    std::vector<double> beta2(p, 0.0), g2, F2;
    score(beta2, g2, F2);
    for (double &x : g2) x = -x;
    tttrlib::DampedNewton<decltype(f)> s_bad;
    const auto rbad = s_bad.step(F2.data(), g2.data(), p, f(beta2.data()), beta2.data(), f);
    check(!rbad.accepted, "negative control: an uphill gradient is never accepted", double(rbad.n_eval));

    {  // RidgeProjector: a tall design with two nearly collinear columns
    const std::size_t rm = 200, rn = 12;
    std::vector<double> D(rm * rn), coef(rn), yv(rm, 0.0), xr(rn), xg;
    for (std::size_t i = 0; i < rm * rn; ++i) D[i] = N(rng);
    for (std::size_t r = 0; r < rm; ++r) D[r * rn + 1] = D[r * rn] + 1e-7 * N(rng);
    for (std::size_t j = 0; j < rn; ++j) coef[j] = N(rng);
    for (std::size_t r = 0; r < rm; ++r) for (std::size_t j = 0; j < rn; ++j) yv[r] += D[r * rn + j] * coef[j];
    tttrlib::RidgeProjector rp;
    const double rel = 1e-8;
    bool rok = rp.factor(D.data(), rm, rn, rel, true) && rp.project(yv.data(), xr.data());
    // the reference: the normal equations with the same lambda, by elimination
    std::vector<double> GtG(rn * rn, 0.0), Dty(rn, 0.0);
    double tr = 0.0;
    for (std::size_t i = 0; i < rn; ++i) for (std::size_t j = 0; j < rn; ++j) for (std::size_t r = 0; r < rm; ++r) GtG[i * rn + j] += D[r * rn + i] * D[r * rn + j];
    for (std::size_t i = 0; i < rn; ++i) { tr += GtG[i * rn + i]; for (std::size_t r = 0; r < rm; ++r) Dty[i] += D[r * rn + i] * yv[r]; }
    for (std::size_t i = 0; i < rn; ++i) GtG[i * rn + i] += rel * tr / double(rn);
    gauss(GtG, Dty, rn, xg);
    double dx = 0.0, xs = 0.0, fit = 0.0, ys = 0.0;
    for (std::size_t j = 0; j < rn; ++j) { dx = std::max(dx, std::fabs(xr[j] - xg[j])); xs = std::max(xs, std::fabs(xg[j])); }
    for (std::size_t r = 0; r < rm; ++r) { double v = 0.0; for (std::size_t j = 0; j < rn; ++j) v += D[r * rn + j] * xr[j]; fit = std::max(fit, std::fabs(v - yv[r])); ys = std::max(ys, std::fabs(yv[r])); }
    check(rok && std::fabs(rp.lambda() - rel * tr / double(rn)) < 1e-12 * tr, "ridge: lambda relative to the mean diagonal", rp.lambda());
    check(rok && dx / xs < 1e-6, "ridge: coefficients = the normal equations by elimination", dx / xs);
    check(rok && fit / ys < 1e-6, "ridge: the fitted target reproduces a target in the span", fit / ys);
    // the collinear pair is shared, not blown up: the unridged problem has no bounded answer
    check(rok && std::fabs(xr[0] + xr[1] - coef[0] - coef[1]) < 1e-3 && std::fabs(xr[0] - xr[1]) < 10.0,
          "ridge: a collinear pair gets bounded coefficients with the right sum", std::fabs(xr[0] - xr[1]));
    std::vector<double> D10(D);
    for (double& v : D10) v *= 10.0;
    std::vector<double> y10(yv), x10(rn);
    for (double& v : y10) v *= 10.0;
    tttrlib::RidgeProjector rp10;
    rp10.factor(D10.data(), rm, rn, rel, true); rp10.project(y10.data(), x10.data());
    double d10 = 0.0;
    for (std::size_t j = 0; j < rn; ++j) d10 = std::max(d10, std::fabs(x10[j] - xr[j]));
    check(d10 / xs < 1e-6, "ridge: relative lambda is invariant to the scale of the design", d10 / xs);
    // negative control: an absolute lambda of the same number is not scale invariant
    tttrlib::RidgeProjector ra, ra10;
    std::vector<double> xa(rn), xa10(rn);
    ra.factor(D.data(), rm, rn, 1.0); ra.project(yv.data(), xa.data());
    ra10.factor(D10.data(), rm, rn, 1.0); ra10.project(y10.data(), xa10.data());
    double da = 0.0;
    for (std::size_t j = 0; j < rn; ++j) da = std::max(da, std::fabs(xa10[j] - xa[j]));
    check(da / xs > 1e-3, "negative control: an absolute lambda is not scale invariant", da / xs);
    }
    return failures;
}
