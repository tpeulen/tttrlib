// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file AccurateFretUncertainty.h
 * \brief Accurate FRET efficiency with propagated uncertainties, distances,
 * per-population summaries, and the bootstrap of the correction factors.
 *
 * Hellenkamp et al. (Nat. Methods 15, 669, 2018) showed that the systematic
 * errors of gamma, alpha and delta, not photon statistics, dominate the
 * spread of FRET efficiencies between laboratories; they are propagated
 * here next to the statistical error.
 */
#ifndef TTTRLIB_ACCURATEFRET_UNCERTAINTY_H
#define TTTRLIB_ACCURATEFRET_UNCERTAINTY_H

#include <vector>

#include "AccurateFret.h"
#include "AccurateFretCalibrate.h"

namespace tttrlib {

/*!
 * \brief Calibration uncertainty of the efficiency, per element.
 *
 * With E = F_da/(F_da + gamma F_dd): d_gamma = |E(1-E)/gamma| sigma_gamma,
 * d_alpha = |(1-E)^2/gamma| sigma_alpha, d_delta = |(1-E)^2 F_aa/(gamma F_dd)|
 * sigma_delta (0 where F_dd = 0 or without F_aa). `systematic` is their
 * quadrature sum, `total` adds `statistical` in quadrature.
 */
struct EfficiencyUncertainty {
    std::vector<double> total, systematic, statistical;
    std::vector<double> d_gamma, d_alpha, d_delta;
};

/*!
 * \param e efficiencies.
 * \param f_dd background-corrected donor signal, same length.
 * \param f_aa background-corrected acceptor-excitation signal, or empty.
 * \param gamma the gamma E was computed with (0 is read as 1).
 * \param sigma_statistical statistical error per element, or empty (zero).
 */
EfficiencyUncertainty efficiency_uncertainty(
    const std::vector<double>& e, const std::vector<double>& f_dd,
    const std::vector<double>& f_aa, double gamma,
    double sigma_gamma = 0.0, double sigma_alpha = 0.0, double sigma_delta = 0.0,
    const std::vector<double>& sigma_statistical = std::vector<double>()
);

/*!
 * \brief Distance R = R0 (1/E - 1)^(1/6) and its error.
 *
 * sigma_R/R = sqrt((sigma_R0/R0)^2 + (sigma_E/(6 E (1-E)))^2). Both are NaN
 * outside 0 < E < 1. Without `sigma_efficiency` only the R0 term counts.
 */
struct DistanceResult {
    std::vector<double> distance;
    std::vector<double> sigma;
};

DistanceResult distance_from_efficiency(
    const std::vector<double>& e, double r0,
    const std::vector<double>& sigma_efficiency = std::vector<double>(),
    double sigma_r0 = 0.0
);

/*!
 * \brief Per-burst accurate E/S, their errors, distances and population summaries.
 *
 * `deviation` (E minus the static line at the burst's lifetime) is filled
 * only when lifetimes and a line are given (`has_deviation`).
 */
struct AccurateFretResult {
    EsResult es;
    std::vector<double> sigma_E, sigma_E_systematic, distance, sigma_distance, deviation;
    bool has_deviation = false;
    std::vector<FretPopulation> populations;
    FretFactors factors;
};

/*!
 * \brief Apply a calibration and report E, S, distances and their errors.
 *
 * Non-finite factor uncertainties count as 0. Populations are the distinct
 * `labels` in ascending order (all bursts one population when empty); a
 * population without a finite E is skipped.
 */
AccurateFretResult accurate_fret(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const FretFactors& factors,
    double sigma_gamma = 0.0, double sigma_alpha = 0.0, double sigma_delta = 0.0,
    double sigma_r0 = 0.0,
    const std::vector<double>& tau_f = std::vector<double>(),
    const std::vector<double>& line_tau_f = std::vector<double>(),
    const std::vector<double>& line_efficiency = std::vector<double>(),
    const std::vector<int>& labels = std::vector<int>()
);

/*!
 * \brief One bootstrap resample of the factor estimators, classes held fixed.
 *
 * Resamples the donor-only, acceptor-only and FRET classes of the last
 * split separately (the statistically correct treatment once the split is
 * accepted) and re-estimates alpha, delta and gamma/beta (the 1/S vs E line,
 * or the lifetime line without two FRET populations), appending to the
 * state's bootstrap samples. `s_d`, `s_a`, `s_f` are positions within each
 * class (0 .. class size - 1) and make the resample reproducible from
 * outside; an empty vector draws them with tttrlib's counter-based
 * generator, seeded by `options.seed`.
 */
void auto_calibrate_bootstrap(
    AutoCalibration& state, const AutoCalibrateOptions& options,
    const std::vector<int>& s_d = std::vector<int>(),
    const std::vector<int>& s_a = std::vector<int>(),
    const std::vector<int>& s_f = std::vector<int>()
);

/*!
 * \brief Bayesian refinement of gamma from labelled FRET populations.
 *
 * gamma_data from `global_es_correction`; its uncertainty `data_sigma` is
 * given (>= 0) or bootstrapped over all bursts (the std of the finite
 * resampled values when more than two, else 10% of |gamma_data|; at least
 * 1e-6). With a prior (sigma >= 0) the posterior is the precision-weighted
 * mean. Everything is NaN when gamma_data is not finite.
 *
 * \param indices optional flat (n_bootstrap, n) resampling indices.
 * \return {gamma_data, beta, data_sigma, gamma_prior, gamma_posterior}.
 */
std::vector<double> refine_gamma(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const std::vector<int>& labels,
    double alpha, double delta, double prior_mu, double prior_sigma,
    double data_sigma = -1.0, int n_bootstrap = 60, unsigned int seed = 0,
    const std::vector<int>& indices = std::vector<int>()
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_UNCERTAINTY_H
