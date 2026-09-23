// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file AccurateFretCalibrate.h
 * \brief Automatic determination of the FRET correction factors from one
 * measurement: the gamma/beta fit, the lifetime route and the
 * self-consistent iteration.
 *
 * `auto_calibrate` runs the whole procedure. It is also exposed as steps
 * (`auto_calibrate_start`, `auto_calibrate_iterate`, `auto_calibrate_finish`)
 * so a caller can report progress and stop early between passes.
 */
#ifndef TTTRLIB_ACCURATEFRET_CALIBRATE_H
#define TTTRLIB_ACCURATEFRET_CALIBRATE_H

#include <string>
#include <vector>

#include "AccurateFret.h"
#include "AccurateFretPopulations.h"

namespace tttrlib {

/*!
 * \brief gamma and beta from the 1/S vs E line over FRET sub-populations.
 *
 * After removing leakage and direct excitation from the raw counts
 * (backgrounds are not subtracted), F_da = i_da - alpha i_dd - delta i_aa,
 * the population means of E = F_da/(i_dd + F_da) and 1/<S>, with
 * S = (i_dd + F_da)/(i_dd + F_da + i_aa), obey 1/S = Omega + Sigma E (Lee et
 * al., Biophys. J. 88, 2939, 2005). The line is fitted by least squares with
 * each population weighted by sqrt(n); gamma = (Omega - 1)/(Omega + Sigma - 1)
 * (NaN when the denominator is below 1e-12 in magnitude) and
 * beta = Omega + Sigma - 1.
 *
 * \return {gamma, beta, Omega, Sigma}.
 * \throws std::invalid_argument when fewer than two distinct labels are given.
 */
std::vector<double> global_es_correction(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const std::vector<int>& labels,
    double alpha = 0.0, double delta = 0.0
);

/*!
 * \brief beta that centres one doubly labelled population at S = target.
 *
 * beta = <F_aa> / (<gamma F_dd + F_da> (1/t - 1)), t = target clipped to
 * [1e-6, 1 - 1e-6]; 1 when either mean is not positive. The fallback when
 * the 1/S vs E fit has fewer than two populations; it assumes 1:1 labelling.
 */
double beta_from_stoichiometry(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const FretFactors& factors, double target = 0.5
);

/*!
 * \brief Per-population gamma from the donor lifetime and a static FRET line.
 *
 * For each label with at least `min_population` bursts whose lifetime,
 * F_dd and F_da are finite, E_line = line(<tau_f>) (linear interpolation of
 * the tabulated line, clamped at its ends) and
 * gamma = (<F_da>/<F_dd>) (1 - E_line)/E_line, kept when E_line lies in
 * [e_min, e_max] and both means are positive. Populations are combined
 * weighted by burst count; `sigma` is the weighted spread over sqrt(number
 * of populations) (NaN for a single population). `gamma` is NaN when no
 * population qualified. Per-population values are in the parallel vectors.
 *
 * \param line_tau_f ascending lifetimes of the line (ns).
 * \param line_efficiency efficiency at each lifetime.
 */
struct LifetimeGamma {
    double gamma = 0.0;
    double sigma = 0.0;
    std::vector<int> labels;
    std::vector<int> n;
    std::vector<double> tau_f;
    std::vector<double> e_line;
    std::vector<double> gammas;
};

LifetimeGamma gamma_from_lifetime(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& tau_f,
    const std::vector<double>& line_tau_f, const std::vector<double>& line_efficiency,
    const std::vector<double>& i_aa, const FretFactors& factors,
    const std::vector<int>& labels, int min_population = 20,
    double e_min = 0.05, double e_max = 0.95
);

/*!
 * \brief gamma, alpha and delta predicted by the light path (scalar two-colour form).
 *
 * gamma = (gR c_ra qy_a)/(gG c_gd qy_d) (NaN when the denominator is at most
 * 1e-12), alpha = (gR c_rd)/(gG c_gd) (0 likewise), delta = ex_ag/ex_ar (0
 * when |ex_ar| is at most 1e-12). c_xy is the emission of dye y reaching
 * detector x, ex_ay the excitation of the acceptor by laser y.
 *
 * \return {gamma, alpha, delta}.
 */
std::vector<double> lightpath_correction_factors(
    double c_gd, double c_rd, double c_ra, double ex_ag, double ex_ar,
    double gG = 1.0, double gR = 1.0, double qy_d = 1.0, double qy_a = 1.0
);

/*!
 * \brief Options of `auto_calibrate`. A prior with a negative or NaN sigma is absent.
 *
 * `gamma_source` is "auto" (E-S fit, lifetime as fallback), "es",
 * "lifetime" or "combined" (mean of the two). Factors are clamped to the
 * `*_lo`/`*_hi` bounds whenever they are written, as the parameter group of
 * the original implementation did. The static FRET line enables the lifetime
 * route when per-burst lifetimes are given.
 */
struct AutoCalibrateOptions {
    std::string gamma_source = "auto";
    int n_iterations = 6;
    double tolerance = 1e-3;
    int n_bootstrap = 0;
    unsigned int seed = 0;
    bool use_priors = true;
    bool assume_one_to_one = true;
    int min_population = 20;
    int max_fret_populations = 3;
    double donor_only_above = 0.75;
    double acceptor_only_below = 0.25;
    double gamma_prior_mu = 0.0, gamma_prior_sigma = -1.0;
    double alpha_prior_mu = 0.0, alpha_prior_sigma = -1.0;
    double delta_prior_mu = 0.0, delta_prior_sigma = -1.0;
    std::vector<double> line_tau_f;
    std::vector<double> line_efficiency;
    double gamma_lo = 0.05, gamma_hi = 20.0;
    double alpha_lo = 0.0, alpha_hi = 1.0;
    double beta_lo = 0.01, beta_hi = 100.0;
    double delta_lo = 0.0, delta_hi = 1.0;
    double bg_lo = 0.0, bg_hi = 1e6;
    double r0_lo = 1.0, r0_hi = 200.0;
};

/*!
 * \brief State and result of the automatic calibration.
 *
 * `factors` are the current (after `finish`: final) factors. `sigma_*` are
 * standard uncertainties (NaN when not estimated). `gamma_*` are the gamma
 * estimates of the last pass: E-S fit, lifetime line (and its spread),
 * data value adopted, light-path prior mean and posterior. `estimated_*`
 * say whether the data identified that factor. `split` is the last
 * population assignment. The `data_*` vectors hold the input bursts.
 */
struct AutoCalibration {
    FretFactors factors;
    double sigma_alpha = 0.0, sigma_delta = 0.0, sigma_gamma = 0.0,
           sigma_beta = 0.0, sigma_r0 = 0.0;
    double gamma_es = 0.0, gamma_lifetime = 0.0, gamma_lifetime_sigma = 0.0,
           gamma_data = 0.0, gamma_prior = 0.0, gamma_posterior = 0.0;
    bool has_lifetime_gamma = false;
    bool estimated_alpha = false, estimated_delta = false, estimated_gamma = false;
    PopulationSplit split;
    bool has_split = false;
    int iterations = 0;
    bool converged = false;
    bool cancelled = false;
    std::vector<std::string> messages;
    std::vector<std::string> iteration_messages;
    std::vector<double> previous;
    std::vector<double> data_dd, data_da, data_aa, data_tau;
};

/*!
 * \brief Validate the bursts, clamp the starting factors and set up a run.
 *
 * \param i_aa acceptor-excitation counts, or empty (no ALEX/PIE channel: every
 *        burst is then treated as doubly labelled).
 * \param tau_f per-burst donor lifetime (ns), or empty.
 * \throws std::invalid_argument when the lengths differ.
 */
AutoCalibration auto_calibrate_start(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const std::vector<double>& tau_f,
    const FretFactors& factors, const AutoCalibrateOptions& options
);

/*!
 * \brief One self-consistency pass: correct, classify, re-estimate.
 *
 * Corrects E/S with the current factors, classifies the bursts, sets alpha
 * from the donor-only and delta from the acceptor-only class, gamma from the
 * source chosen by `gamma_source`, and beta from the 1/S vs E fit, or from
 * S = 0.5 when there is a single FRET population and `assume_one_to_one`.
 *
 * \return true when no factor changed by `tolerance` or more.
 */
bool auto_calibrate_iterate(AutoCalibration& state, const AutoCalibrateOptions& options);

/*!
 * \brief Record that the caller stopped the iteration after the current pass.
 */
void auto_calibrate_cancel(AutoCalibration& state);

/*!
 * \brief Close the run: keep the last pass's messages, fall back to the
 * lifetime spread for sigma_gamma, and combine gamma/alpha/delta with their
 * light-path priors (precision-weighted; a factor the data did not identify
 * takes the prior value and width).
 */
void auto_calibrate_finish(AutoCalibration& state, const AutoCalibrateOptions& options);

/*!
 * \brief The whole calibration in one call (start, iterate until converged
 * or `n_iterations`, finish).
 */
AutoCalibration auto_calibrate(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const std::vector<double>& tau_f,
    const FretFactors& factors, const AutoCalibrateOptions& options
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_CALIBRATE_H
