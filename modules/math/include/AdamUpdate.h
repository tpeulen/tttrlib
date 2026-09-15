/*!
 * @file AdamUpdate.h
 * @brief One Adam step (Kingma & Ba, "Adam: A Method for Stochastic
 *        Optimization", ICLR 2015, Algorithm 1), with its state.
 *
 * Header-only and std-only, so imp.bff carries it as a verbatim copy. The one
 * implementation for every Adam in the stack: network training, the model-search
 * self-play value model, and a first-order pre-optimiser in front of a
 * Fisher-scoring fit.
 *
 * For each parameter, with gradient `g` of the objective to MINIMISE and step `t`
 * (counted from 1): `m = b1 m + (1 - b1) g`, `v = b2 v + (1 - b2) g^2`,
 * `p -= lr (m / (1 - b1^t)) / (sqrt(v / (1 - b2^t)) + eps)`. The two `1 - b^t` are
 * the bias corrections that keep the first steps from being too small while `m`
 * and `v` still remember their zero start.
 *
 * **What it is good for, measured.** On the CBM56 Bayesian decay fit (136
 * parameters), 1500 full-gradient Adam steps from a start in a poor basin followed
 * by Fisher scoring ended 0-6 nats below the best optimum of three starts where
 * Fisher scoring alone ended 46-78 nats below -- an escape, not a solution, and
 * about twenty times the cost of the scoring fit (ucfret, 2026-09-15).
 *
 * Written 2026-09-15.
 */
#ifndef TTTRLIB_ADAMUPDATE_H
#define TTTRLIB_ADAMUPDATE_H

#include <cmath>
#include <cstddef>
#include <vector>

namespace tttrlib {

//! The moments and the step count Adam carries between steps.
struct AdamState {
    std::vector<double> m, v;
    long t = 0;
    //! Zero state for `n` parameters.
    void reset(std::size_t n) { m.assign(n, 0.0); v.assign(n, 0.0); t = 0; }
};

/*!
 * @brief Advance `state` by one step and move `p` (n parameters) against `g`.
 * @param p[in,out] parameters
 * @param g[in] gradient of the objective being minimised
 * @param n[in] number of parameters
 * @param state[in,out] moments and step count (sized by `reset(n)` first)
 * @param lr[in] step size
 * @param beta1[in] first-moment decay
 * @param beta2[in] second-moment decay
 * @param eps[in] added to the denominator
 */
inline void adam_update(double *p, const double *g, std::size_t n, AdamState &state, double lr,
                        double beta1 = 0.9, double beta2 = 0.999, double eps = 1e-8) {
    if (state.m.size() != n) state.reset(n);
    ++state.t;
    const double bc1 = 1.0 - std::pow(beta1, double(state.t));
    const double bc2 = 1.0 - std::pow(beta2, double(state.t));
    double *m = state.m.data(), *v = state.v.data();
    for (std::size_t i = 0; i < n; ++i) {
        m[i] = beta1 * m[i] + (1.0 - beta1) * g[i];
        v[i] = beta2 * v[i] + (1.0 - beta2) * g[i] * g[i];
        p[i] -= lr * (m[i] / bc1) / (std::sqrt(v[i] / bc2) + eps);
    }
}

}  // namespace tttrlib

#endif  // TTTRLIB_ADAMUPDATE_H
