// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file AccurateFretPopulations.h
 * \brief Automatic donor-only / acceptor-only / FRET gating and the
 * reference-sample estimators of alpha and delta.
 *
 * A one-dimensional Gaussian mixture fitted to the stoichiometry separates
 * the three species of an ALEX/PIE measurement without hand-drawn gates; a
 * second mixture over the efficiency splits the doubly labelled bursts into
 * the sub-populations the gamma/beta fit needs.
 */
#ifndef TTTRLIB_ACCURATEFRET_POPULATIONS_H
#define TTTRLIB_ACCURATEFRET_POPULATIONS_H

#include <string>
#include <vector>

namespace tttrlib {

/*!
 * \brief A fitted one-dimensional Gaussian mixture, components sorted by mean.
 *
 * `responsibilities` is row-major `(n_samples, n_components)` over the finite
 * samples in input order; `labels` is the most likely component per finite
 * sample. `bic_k` / `bic_values` list the BIC of every component number
 * tried by `best_gaussian_mixture_1d` (empty for a single fit).
 */
struct MixtureResult {
    int n_components = 0;
    std::vector<double> weights;
    std::vector<double> means;
    std::vector<double> sigmas;
    std::vector<double> responsibilities;
    std::vector<int> labels;
    double log_likelihood = 0.0;
    double bic = 0.0;
    int n_iter = 0;
    std::vector<int> bic_k;
    std::vector<double> bic_values;
};

/*!
 * \brief Fit a one-dimensional Gaussian mixture by expectation-maximisation.
 *
 * Deterministic. Two starts are tried and the higher final log-likelihood is
 * kept: quantile-spaced means (they follow the density and handle
 * overlapping components) and, for more than one component, range-spaced
 * means (they reach a sparse population that a dominant one would otherwise
 * swallow). Every start has equal weights and the width
 * max(std(x) / k, sigma_floor). The width update is additive-ridged by
 * sigma_floor², so no component collapses onto one sample. EM stops when the
 * total log-likelihood gains less than `tolerance` in one iteration.
 *
 * The width update reproduces chisurf's spherical estimator exactly,
 * including its weighting of squared residuals by the squared
 * responsibility; see the corrections README.
 *
 * \param x samples; non-finite values are ignored.
 * \param n_components number of components (at least 1).
 * \param n_iterations maximum EM iterations.
 * \param tolerance convergence threshold on the log-likelihood gain.
 * \param sigma_floor lower bound of the component widths.
 * \param init "auto" (both starts), "quantile" or "range".
 * \throws std::invalid_argument when there is no finite sample or more
 *         components than samples.
 */
MixtureResult gaussian_mixture_1d(
    const std::vector<double>& x, int n_components,
    int n_iterations = 300, double tolerance = 1e-7,
    double sigma_floor = 1e-3, const std::string& init = "auto"
);

/*!
 * \brief Choose the component number of a 1-D mixture by BIC.
 *
 * Tries 1 .. max_components components while there are at least 5 samples
 * per component; a fit with a component weight below `min_weight` (for more
 * than one component) is not eligible. The lowest BIC among the eligible fits
 * wins; the one-component fit is the fallback.
 */
MixtureResult best_gaussian_mixture_1d(
    const std::vector<double>& x, int max_components = 4, double min_weight = 0.02
);

/*!
 * \brief Donor-only, acceptor-only and FRET bursts and FRET sub-population labels.
 *
 * Masks are per burst (1 = member). `fret_labels` is the FRET sub-population
 * index per burst, -1 for every burst outside the FRET class. `threshold_lo`
 * and `threshold_hi` are the stoichiometry cuts of the FRET class; `method`
 * is "mixture", "threshold", "none" or "mixture_nd". The `component_*`
 * vectors describe the stoichiometry mixture that placed the cuts (empty for
 * "threshold"); for "mixture_nd" `component_means`/`component_sigmas` are
 * row-major (n_components, n_dimensions) in the units of `dimensions`.
 * `fret_probabilities` is row-major (n_bursts, n_fret_populations): each
 * FRET burst's probability of belonging to each FRET sub-population (rows of
 * other bursts are 0). The multidimensional gating fills it from the
 * mixture's responsibilities; the stoichiometry gating with one-hot rows.
 *
 * Density-based gating (`method` "hdbscan") also fills `noise` (bursts HDBSCAN
 * left unclaimed: in no class, excluded from the factors), `cluster_labels`
 * (cluster per burst, -1 for noise, outliers and bursts without a declared
 * value) and `membership` (HDBSCAN membership strength, a rank within the
 * cluster, 0 for noise). The multidimensional gating of `auto_calibrate`
 * pre-cleans its dimensions (`flag_dimension_outliers`): `outlier` marks the
 * bursts it removed (also in no class, and not counted as noise), and
 * `outlier_dimensions` / `outlier_range` / `outlier_fence` / `outlier_lo` /
 * `outlier_hi` give, per dimension, the bursts outside the physical range, the
 * bursts outside the robust fence and the fence itself. All of these are empty
 * when the step did not run.
 */
struct PopulationSplit {
    std::vector<int> donor_only;
    std::vector<int> acceptor_only;
    std::vector<int> fret;
    std::vector<int> fret_labels;
    double threshold_lo = 0.25;
    double threshold_hi = 0.75;
    std::string method = "threshold";
    std::vector<double> component_means;
    std::vector<double> component_weights;
    std::vector<double> component_sigmas;
    std::vector<int> bic_k;
    std::vector<double> bic_values;
    std::vector<std::string> dimensions;
    int n_fret_populations = 0;
    std::vector<double> fret_probabilities;
    std::vector<int> noise;
    std::vector<int> cluster_labels;
    std::vector<double> membership;
    std::vector<int> outlier;
    std::vector<std::string> outlier_dimensions;
    std::vector<int> outlier_range;
    std::vector<int> outlier_fence;
    std::vector<double> outlier_lo;
    std::vector<double> outlier_hi;
};

/*!
 * \brief Find donor-only, acceptor-only and FRET bursts without manual gates.
 *
 * A BIC-selected mixture is fitted to the finite stoichiometries (when there
 * are at least 15). Components centred at or above `donor_only_above` are
 * donor-only, at or below `acceptor_only_below` acceptor-only, the rest FRET.
 * The FRET class is cut at the midpoints between the outermost FRET component
 * and the nearest reference component (completeness); the reference classes
 * are further restricted to within `reference_sigma` widths of their own
 * component (purity, since a doubly labelled burst in them biases alpha and
 * delta directly; 0 disables it). Without a FRET component, or with
 * `method = "threshold"`, the fixed cuts are used. A reference class with
 * fewer than `min_population` bursts is emptied. With an efficiency, FRET
 * bursts are split by `split_fret_subpopulations`; otherwise they share
 * label 0.
 *
 * \param stoichiometry per-burst S.
 * \param efficiency per-burst E, or empty.
 */
PopulationSplit classify_es_populations(
    const std::vector<double>& stoichiometry,
    const std::vector<double>& efficiency = std::vector<double>(),
    double donor_only_above = 0.75, double acceptor_only_below = 0.25,
    int max_components = 4, int max_fret_populations = 3,
    int min_population = 20, double reference_sigma = 2.0,
    const std::string& method = "auto"
);

/*!
 * \brief Split doubly labelled bursts into efficiency sub-populations.
 *
 * A BIC-selected mixture over the finite efficiencies; components holding
 * fewer than max(min_population, ceil(min_fraction n)) bursts, or closer
 * than `min_separation` to the previously kept one, are merged into the
 * nearest kept component. Fewer than two kept components, fewer than
 * 2 min_population finite bursts or `max_populations < 2` give all zeros.
 * Non-finite bursts follow component 0.
 *
 * \return sub-population index per burst, numbered by increasing efficiency.
 */
std::vector<int> split_fret_subpopulations(
    const std::vector<double>& efficiency,
    int max_populations = 3, int min_population = 20,
    double min_separation = 0.05, double min_fraction = 0.1
);

/*!
 * \brief Donor leakage alpha = <i_da - bg_da> / <i_dd - bg_dd> of donor-only bursts.
 *
 * \return alpha, or 0 when the mean donor signal is exactly 0.
 */
double leakage_from_donor_only(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    double bg_dd = 0.0, double bg_da = 0.0
);

/*!
 * \brief Direct excitation delta of acceptor-only bursts.
 *
 * delta = <i_da - bg_da - alpha (i_dd - bg_dd)> / <i_aa - bg_aa>; the leakage
 * term is dropped when `i_dd` is empty or alpha is 0.
 *
 * \return delta, or 0 when the mean acceptor signal is exactly 0.
 */
double direct_excitation_from_acceptor_only(
    const std::vector<double>& i_da, const std::vector<double>& i_aa,
    const std::vector<double>& i_dd = std::vector<double>(),
    double alpha = 0.0, double bg_dd = 0.0, double bg_da = 0.0, double bg_aa = 0.0
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_POPULATIONS_H
