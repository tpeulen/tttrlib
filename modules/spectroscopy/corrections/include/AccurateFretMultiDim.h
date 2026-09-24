// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file AccurateFretMultiDim.h
 * \brief Population gating over every per-burst dimension that is present.
 *
 * The stoichiometry alone separates donor-only, acceptor-only and doubly
 * labelled molecules; the donor lifetime, the acceptor lifetime and the
 * anisotropies separate what S cannot (two FRET species of similar E but
 * different lifetime, a donor-only population whose S is smeared by
 * background). The caller declares which columns take part; the mixture is
 * a diagonal-covariance Gaussian mixture over those columns, each
 * standardised to zero mean and unit variance.
 */
#ifndef TTTRLIB_ACCURATEFRET_MULTIDIM_H
#define TTTRLIB_ACCURATEFRET_MULTIDIM_H

#include <string>
#include <vector>

#include "AccurateFretPopulations.h"

namespace tttrlib {

/*!
 * \brief A fitted diagonal-covariance Gaussian mixture.
 *
 * `means` and `sigmas` are row-major (n_components, n_dims) in the units of
 * the input; components are sorted by the mean of the first dimension.
 * `responsibilities` is row-major (n_rows, n_components) and `labels` the
 * most likely component, for every input row. A non-finite value is
 * missing and marginalised (the diagonal density factorises), so a row takes
 * part with the dimensions it has -- a donor-only burst has no acceptor
 * lifetime; rows without any finite value have NaN responsibilities and
 * label -1. `bic_k`/`bic_values` list every
 * component number tried by `best_gaussian_mixture_nd`.
 */
struct MixtureNdResult {
    int n_components = 0;
    int n_dims = 0;
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
 * \brief Fit a diagonal-covariance Gaussian mixture by EM (standard updates).
 *
 * Deterministic: for each dimension the rows are ordered along it and cut
 * into k equal groups, whose means and standard deviations start the
 * components; each start runs 20 iterations and the most likely one is run to
 * convergence (per-sample log-likelihood gain below `tolerance`), since
 * species can differ in any single dimension. Widths are ridged by
 * `sigma_floor` standard deviations of each dimension.
 *
 * \param x row-major (n_rows, n_dims).
 * \throws std::invalid_argument when fewer usable rows than components remain.
 */
MixtureNdResult gaussian_mixture_nd(
    const std::vector<double>& x, int n_rows, int n_dims, int n_components,
    int n_iterations = 300, double tolerance = 1e-7, double sigma_floor = 1e-3
);

/*!
 * \brief BIC over 1 .. max_components (at least 5 finite rows per component);
 * fits with a component weight below `min_weight` are not eligible.
 */
MixtureNdResult best_gaussian_mixture_nd(
    const std::vector<double>& x, int n_rows, int n_dims,
    int max_components = 6, double min_weight = 0.02
);

/*!
 * \brief Donor-only, acceptor-only and FRET bursts from a multidimensional mixture.
 *
 * `names` label the columns of `x` from the declared vocabulary: "S", "E",
 * "tau_d" (donor lifetime), "tau_a" (acceptor lifetime), "r_d", "r_a"
 * (anisotropies) or any other per-burst quantity. Each component is labelled
 * by its centre: with "S", donor-only at or above `donor_only_above`,
 * acceptor-only at or below `acceptor_only_below`, FRET otherwise; without
 * "S", a component is donor-only when its E centre is below 0.1 and, with
 * "tau_d", its lifetime centre is at least 90% of the longest one; there is
 * then no acceptor-only class. A burst joins its most likely component's
 * class; the reference classes additionally need a probability of at least
 * `min_probability` (purity). A FRET component closer to a larger one than
 * two pooled widths (sum over dimensions of delta^2/(sigma_a^2 + sigma_b^2)
 * below 4) is the same species and is merged into it, as is one with fewer
 * than `min_population` bursts (into the nearest by that distance); the kept
 * ones, ordered by E (the first dimension without E), are the FRET
 * sub-populations, and `fret_probabilities` holds each FRET burst's
 * normalised probability for each of them. Reference classes smaller than
 * `min_population` are emptied. Missing values are marginalised; a burst
 * with no finite declared value belongs to no class.
 */
PopulationSplit classify_populations_nd(
    const std::vector<double>& x, int n_rows, const std::vector<std::string>& names,
    double donor_only_above = 0.75, double acceptor_only_below = 0.25,
    int max_components = 6, int min_population = 20, double min_probability = 0.9
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_MULTIDIM_H
