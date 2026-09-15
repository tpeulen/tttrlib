/*!
 * @file PeriodicDecayKernel.h
 * @brief The bin-integrated periodic decay kernel: what a histogram channel
 *        receives from light absorbed uniformly within one channel, under
 *        excitation that repeats every period.
 *
 * Header-only and std-only, so a library that cannot link tttrlib (imp.bff)
 * carries it as a verbatim copy.
 *
 * **What it is.** Light absorbed uniformly within channel 0 (width `dt`) decays
 * with a normalised density `f`, the detection is integrated over each channel,
 * and every earlier pulse, `P = n_period * dt` apart, adds its tail. Channel `k`
 * of the period receives
 *
 *     K(k) = (1/dt) int_0^dt du  sum_{m >= 0} int_{k dt}^{(k+1) dt} f(t - u + m P) dt
 *
 * and `sum_k K(k) = 1` over one period, exactly. Two densities:
 *
 * - `order = 0`: `f(t) = exp(-t/tau) / tau`, one exponential. In closed form,
 *   with `s = tau/dt` and `q = exp(-dt/tau)`,
 *   `K(k >= 1) = s (1-q)^2 q^(k-1) / (1 - exp(-P/tau))`.
 * - `order = 1`: `f(t) = t exp(-t/tau) / tau^2`, the rise-and-decay a two-state
 *   excited-state kinetics produces when its two rates coincide (transfer from a
 *   donor whose quenched rate equals the acceptor's) -- the limit a sum of two
 *   exponentials cannot represent.
 *
 * **How.** With `G` the second antiderivative of `f` from 0, the inner integrals
 * are a second difference of `G` over `dt`. `G` is linear plus
 * `exp(-x/tau) (A + B x)` (order 0: A = tau, B = 0; order 1: A = 2 tau, B = 1); the
 * linear part has no second difference, and the sum over earlier pulses of the
 * exponential part is geometric (`S0 = 1/(1-qP)`, `S1 = qP/(1-qP)^2`,
 * `qP = exp(-P/tau)`). The second difference is formed algebraically
 * (`exp(-x/tau) [(A + B x)(1-q)^2 - B dt (1-q^2)] / q`, `1 - q` through `expm1`),
 * never as a difference of large numbers, so long lifetimes lose no digits; and
 * `K(0) = 1 - sum_{k >= 1} K(k)` avoids `exp(+dt/tau)` for short ones.
 *
 * Written 2026-09-15. The order-0 form is the kernel used since 2026-09-14 by the
 * CBM56 analysis (ucfret `s53_phase1_pseudolik.periodic_columns(kernel='exact')`,
 * imp.bff's periodic basis), where the trapezoid kernel it replaced was off by
 * 1-1.6 % of the peak at rotational times of tens of picoseconds.
 */
#ifndef TTTRLIB_PERIODICDECAYKERNEL_H
#define TTTRLIB_PERIODICDECAYKERNEL_H

#include <cmath>

/*!
 * @brief Fill `out[0 .. n_period-1]` with the bin-integrated periodic kernel.
 * @param out[out] `n_period` values, summing to one
 * @param n_period[in] channels per excitation period, >= 1
 * @param dt[in] channel width (same unit as `tau`)
 * @param tau[in] lifetime, > 0
 * @param order[in] 0 for exp(-t/tau), 1 for t exp(-t/tau)
 */
inline void periodic_decay_kernel(double *out, int n_period, double dt, double tau, int order = 0) {
    if (n_period <= 0) return;
    // q and 1 - q each computed directly: 1 - q through expm1 keeps long lifetimes'
    // digits, q through exp keeps short ones' (1 - (1 - q) loses q when q ~ 1e-9)
    const double one_q = -std::expm1(-dt / tau);
    const double q = std::exp(-dt / tau);
    const double one_qP = -std::expm1(-double(n_period) * dt / tau);
    const double qP = std::exp(-double(n_period) * dt / tau);
    const double S0 = 1.0 / one_qP;
    const double S1 = qP / (one_qP * one_qP);
    const double P = double(n_period) * dt;
    const double A = (order == 0) ? tau : 2.0 * tau;
    const double B = (order == 0) ? 0.0 : 1.0;
    double rest = 0.0;
    for (int k = 1; k < n_period; ++k) {
        const double x = double(k) * dt;
        // sum over earlier pulses of the second difference of exp(-x/tau)(A + B x), / dt
        const double lin = (A + B * x) * S0 + B * P * S1;
        const double value = std::exp(-x / tau) / q
                             * (lin * one_q * one_q - B * dt * one_q * (1.0 + q) * S0) / dt;
        out[k] = value;
        rest += value;
    }
    out[0] = 1.0 - rest;
}

#endif  // TTTRLIB_PERIODICDECAYKERNEL_H
