// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file DecayPatternFit.h
 * \brief General N-arbitrary-pattern fit: decompose a decay into a
 * non-negative combination of caller-supplied fixed reference patterns.
 *
 * `DecayFit26` already fits a mixture, but of exactly two fixed patterns with
 * one free mixing fraction. `DecayFitProblem::patterns` carries an arbitrary
 * *list* of fixed patterns but, before this file, no built-in model read it:
 * it was scaffolding for the C-ABI plugin interface only. This is that
 * consumer -- N patterns in, N non-negative amplitudes out, no constraint
 * that they sum to one.
 *
 * Two ways to resolve the amplitudes, selected by `PatternFitMode`, both
 * exact (KKT) solutions via `tttrlib::nnls` (Nnls.h):
 *
 *  - `kNone`: plain non-negative least squares -- no regularisation, the
 *    amplitudes that best explain the data and nothing else.
 *  - `kTikhonov`: L2-regularised, non-negative, `||Ax-b||^2 + lambda*||x||^2`
 *    -- non-negative least squares on the augmented system `[A; sqrt(lambda) I]`
 *    against `[b; 0]`, which is that objective exactly. Shrinks every amplitude
 *    toward zero, useful when patterns are collinear and the unregularised
 *    solution is ill-conditioned.
 *
 * Column `k` of the design is pattern `k`, row `i` is bin `i`. Maximum-entropy
 * regularisation lives in imp.bff (`IMP.bff.maxent_invert`).
 */
#ifndef TTTRLIB_DECAYPATTERNFIT_H
#define TTTRLIB_DECAYPATTERNFIT_H

// Validation: A/B-TESTED -- kNone vs scipy.optimize.nnls; kTikhonov vs scipy.optimize.nnls on the augmented
//   system. test/python/decayfit/test_decay_pattern_fit.py.
//   Register: okf/testing/algorithm-validation.md

#include <vector>

namespace tttrlib {

enum class PatternFitMode {
    kNone = 0,      ///< plain NNLS, no regularisation
    kTikhonov = 1   ///< L2-regularised, non-negative
};

/*! Result of a pattern-mixture fit. */
struct PatternFitResult {
    std::vector<double> amplitudes;  ///< one non-negative weight per pattern
    double chisq = 0.0;              ///< ||sum_k amplitude_k * pattern_k - data||^2
    bool success = false;
};

/*!
 * \brief Fit non-negative amplitudes of N fixed patterns against `data`.
 *
 * \param data measured histogram, length n_bins.
 * \param patterns N fixed reference patterns, each length n_bins (e.g.
 *        `DecayFitProblem::patterns`, or an IRF/background/donor-only set
 *        assembled by the caller).
 * \param mode which regularisation to apply -- see the file docstring.
 * \param reg_strength `lambda` for kTikhonov, non-negative. Ignored by kNone.
 * \param max_iter, tol iteration cap and dual-feasibility tolerance of `nnls`.
 * \throws std::invalid_argument when a pattern's length differs from
 *         data.size() or `reg_strength` is negative under kTikhonov.
 */
PatternFitResult decay_pattern_fit(
    const std::vector<double>& data,
    const std::vector<std::vector<double>>& patterns,
    PatternFitMode mode,
    double reg_strength = 0.0,
    int max_iter = 200,
    double tol = 1e-8
);

} // namespace tttrlib

#endif // TTTRLIB_DECAYPATTERNFIT_H
