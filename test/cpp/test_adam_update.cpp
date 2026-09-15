// adam_update against Kingma & Ba's Algorithm 1, written out independently.
//
// Checks: 200 steps on random gradients agree bit for bit with the paper's
// algorithm transcribed below; on a quadratic whose curvatures span six decades
// Adam reaches the minimum; and a version without the bias correction must take a
// different first step (the check is not blind to it).
//
//     c++ -std=c++17 -O2 -I modules/math/include -o t test/cpp/test_adam_update.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "AdamUpdate.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-62s %.3e\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

int main() {
    const std::size_t n = 17;
    std::mt19937_64 rng(3);
    std::normal_distribution<double> N(0.0, 1.0);
    // 1. against Algorithm 1
    std::vector<double> p(n), q(n), m(n, 0.0), v(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) p[i] = q[i] = N(rng);
    tttrlib::AdamState s;
    const double a = 1e-3, b1 = 0.9, b2 = 0.999, e = 1e-8;
    double worst = 0.0;
    for (int t = 1; t <= 200; ++t) {
        std::vector<double> g(n);
        for (double &x : g) x = N(rng) * std::exp(N(rng));
        tttrlib::adam_update(p.data(), g.data(), n, s, a, b1, b2, e);
        for (std::size_t i = 0; i < n; ++i) {                 // Algorithm 1, line by line
            m[i] = b1 * m[i] + (1.0 - b1) * g[i];
            v[i] = b2 * v[i] + (1.0 - b2) * g[i] * g[i];
            const double mhat = m[i] / (1.0 - std::pow(b1, t));
            const double vhat = v[i] / (1.0 - std::pow(b2, t));
            q[i] = q[i] - a * mhat / (std::sqrt(vhat) + e);
        }
        for (std::size_t i = 0; i < n; ++i) worst = std::max(worst, std::fabs(p[i] - q[i]));
    }
    check(worst == 0.0, "bit for bit with Algorithm 1 over 200 steps", worst);
    // 2. a badly scaled quadratic, f = 0.5 sum c_i x_i^2, c from 1e-3 to 1e3
    std::vector<double> x(n, 1.0), c(n);
    for (std::size_t i = 0; i < n; ++i) c[i] = std::pow(10.0, -3.0 + 6.0 * double(i) / double(n - 1));
    tttrlib::AdamState sq;
    double lr = 0.05;
    for (int t = 1; t <= 20000; ++t) {
        std::vector<double> g(n);
        for (std::size_t i = 0; i < n; ++i) g[i] = c[i] * x[i];
        if (t == 10000) lr = 0.005;
        tttrlib::adam_update(x.data(), g.data(), n, sq, lr);
    }
    double xmax = 0.0; for (double xi : x) xmax = std::max(xmax, std::fabs(xi));
    check(xmax < 1e-2, "reaches the minimum of a quadratic spanning six decades", xmax);
    // 3. negative control: the first step without the bias correction differs
    std::vector<double> y(1, 0.0), z(1, 0.0); const double g0 = 0.3;
    tttrlib::AdamState s1; tttrlib::adam_update(y.data(), &g0, 1, s1, a, b1, b2, e);
    const double m0 = (1.0 - b1) * g0, v0 = (1.0 - b2) * g0 * g0;
    z[0] -= a * m0 / (std::sqrt(v0) + e);
    check(std::fabs(y[0] - z[0]) > 1e-4, "negative control: no bias correction takes a different first step", std::fabs(y[0] - z[0]));
    return failures;
}
