// The bin-integrated periodic decay kernel against an independent integration.
//
// For both orders, and lifetimes from a twentieth of a channel to twice the
// period: every channel against a direct numerical integration (the excitation
// time integrated by a 4096-point midpoint rule over the channel, the detection
// interval analytically through the density's own CDF, the earlier pulses summed
// term by term until they vanish); the sum over a period is one; order 0 equals
// the textbook closed form s (1-q)^2 q^(k-1) / (1 - exp(-P/tau)). And checks that
// must fail: a kernel that samples the density at channel centres, and order 0
// used where order 1 is meant, both miss the integration by far more than the
// tolerance.
//
//     c++ -std=c++17 -O2 -I modules/spectroscopy/decay/include \
//         -o t test/cpp/test_periodic_decay_kernel.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15.
#include <cmath>
#include <cstdio>
#include <vector>

#include "PeriodicDecayKernel.h"

static int failures = 0;
static void check(bool ok, const char *what, double value, double bound) {
    std::printf("%s %-58s %.3e (%s %.1e)\n", ok ? "PASS" : "FAIL", what, value, ok ? "<" : "not <", bound);
    if (!ok) ++failures;
}
static void check_above(bool ok, const char *what, double value, double bound) {
    std::printf("%s %-58s %.3e (must exceed %.1e)\n", ok ? "PASS" : "FAIL", what, value, bound);
    if (!ok) ++failures;
}

// CDF of the normalised density, 0 below 0
static double cdf(double x, double tau, int order) {
    if (x <= 0.0) return 0.0;
    const double e = std::exp(-x / tau);
    return order == 0 ? 1.0 - e : 1.0 - e * (1.0 + x / tau);
}

static std::vector<double> integrate(int n, double dt, double tau, int order) {
    std::vector<double> k(n, 0.0);
    const int M = 4096;
    const double P = n * dt;
    for (int e = 0; e < M; ++e) {
        const double u = (e + 0.5) / M * dt;
        for (int j = 0; j < n; ++j) {
            double s = 0.0;
            for (int m = 0; m < 100000; ++m) {
                const double a = j * dt - u + m * P, b = a + dt;
                const double t = cdf(b, tau, order) - cdf(a, tau, order);
                s += t;
                if (m > 0 && t < 1e-18) break;
            }
            k[j] += s / M;
        }
    }
    return k;
}

int main() {
    const int n = 64;
    const double dt = 0.064;
    double worst[2] = {0.0, 0.0}, worst_sum = 0.0, worst_closed = 0.0, naive = 1e300, swapped = 1e300;
    for (double tau : {dt / 20.0, dt / 3.0, dt, 0.3, 1.5, 4.0, 2.0 * n * dt}) {
        for (int order = 0; order < 2; ++order) {
            std::vector<double> K(n);
            periodic_decay_kernel(K.data(), n, dt, tau, order);
            const std::vector<double> ref = integrate(n, dt, tau, order);
            double peak = 0.0, err = 0.0, sum = 0.0;
            for (int j = 0; j < n; ++j) { peak = std::max(peak, ref[j]); err = std::max(err, std::fabs(K[j] - ref[j])); sum += K[j]; }
            worst[order] = std::max(worst[order], err / peak);
            worst_sum = std::max(worst_sum, std::fabs(sum - 1.0));
            // a kernel sampled at channel centres, normalised: not the bin integral
            std::vector<double> c(n); double sc = 0.0;
            for (int j = 0; j < n; ++j) { const double x = (j + 0.5) * dt; c[j] = (order == 0 ? 1.0 : x / tau) * std::exp(-x / tau); sc += c[j]; }
            double ne = 0.0; for (int j = 0; j < n; ++j) ne = std::max(ne, std::fabs(c[j] / sc - ref[j]));
            if (tau <= 1.5) naive = std::min(naive, ne / peak);
            if (order == 1) {
                std::vector<double> K0(n); periodic_decay_kernel(K0.data(), n, dt, tau, 0);
                double se = 0.0; for (int j = 0; j < n; ++j) se = std::max(se, std::fabs(K0[j] - ref[j]));
                swapped = std::min(swapped, se / peak);
            } else {
                const double s = tau / dt, one_q = -std::expm1(-dt / tau), q = std::exp(-dt / tau), wrap = 1.0 / (-std::expm1(-n * dt / tau));
                double ce = 0.0;
                for (int j = 1; j < n; ++j) ce = std::max(ce, std::fabs(K[j] - s * one_q * one_q * std::pow(q, j - 1) * wrap) / peak);
                worst_closed = std::max(worst_closed, ce);
            }
        }
    }
    check(worst[0] < 1e-7, "order 0 against the integration (relative to the peak)", worst[0], 1e-7);
    check(worst[1] < 1e-7, "order 1 against the integration (relative to the peak)", worst[1], 1e-7);
    check(worst_sum < 1e-12, "each kernel sums to one over the period", worst_sum, 1e-12);
    check(worst_closed < 1e-12, "order 0 equals the closed form s(1-q)^2 q^(k-1)/(1-qP)", worst_closed, 1e-12);
    check_above(naive > 1e-3, "negative control: centre-sampled kernel misses", naive, 1e-3);
    check_above(swapped > 1e-2, "negative control: order 0 in place of order 1 misses", swapped, 1e-2);
    return failures;
}
