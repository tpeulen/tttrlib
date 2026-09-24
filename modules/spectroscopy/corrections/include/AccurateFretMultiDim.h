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
 * then no acceptor-only class. With "S" and "tau_d" a donor-only component
 * also needs a lifetime centre of at least 90% of the longest donor-only one
 * (tau_D ~ tau_D(0)); a shorter one is in no class. A burst joins its most
 * likely component's class; the reference classes additionally need a
 * probability of at least `min_probability` summed over the components of
 * that class (purity). A FRET component closer to a larger one than
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

/*!
 * \brief Per-dimension outliers, found before any scaling.
 *
 * Per declared dimension: `n_range` bursts outside the physical range,
 * `n_fence` further bursts outside the robust fence [lo, hi]
 * (NaN when no fence applies). `outlier` is 1 for a burst flagged in any
 * dimension.
 */
struct DimensionOutliers {
    std::vector<int> outlier;
    std::vector<std::string> dimensions;
    std::vector<int> n_range;
    std::vector<int> n_fence;
    std::vector<double> lo;
    std::vector<double> hi;
};

/*!
 * \brief Flag the bursts whose declared values are impossible or far out.
 *
 * Two tests per dimension, in this order, so the second never sees what the
 * first removed: (1) the physical range -- "S"/"E" in [es_lo, es_hi], "tau_d"/
 * "tau_a" in (0, tau_max] ns, "r_d"/"r_a" in [r_lo, r_hi], every other name
 * finite -- with +-inf always out; (2) for every dimension except S and E, the
 * fence [q_lo - fence_k w, q_hi + fence_k w], w = q_hi - q_lo, of the values
 * that passed (1), q_lo and q_hi their `quantile` and 1 - `quantile` quantiles
 * (`fence_k` <= 0 disables it). Wide quantiles, not quartiles: the gating
 * columns are multimodal, and a quartile (Tukey) fence around a dominant FRET
 * lifetime cuts off the whole donor-only population; any population holding
 * more than `quantile` of the bursts lies inside [q_lo, q_hi]. S and E get no
 * fence: they are bounded. NaN is a missing value, not an outlier (a
 * donor-only burst has no acceptor lifetime).
 *
 * \param x row-major (n_rows, names.size()).
 */
DimensionOutliers flag_dimension_outliers(
    const std::vector<double>& x, int n_rows, const std::vector<std::string>& names,
    double es_lo = -0.2, double es_hi = 1.2, double tau_max = 20.0,
    double r_lo = -0.5, double r_hi = 1.0, double fence_k = 1.0, double quantile = 0.025
);

/*!
 * \brief Donor-only, acceptor-only and FRET bursts by density-based clustering.
 *
 * HDBSCAN (modules/math, Cluster.h) over the declared columns, each scaled by
 * its robust width (IQR / 1.349 of its finite values; the standard deviation,
 * then 1, when that is 0) after centring on its median. A missing value takes
 * a sentinel three widths below the column's smallest value, so "has no
 * acceptor lifetime" is a coordinate of its own rather than a hole. At most
 * `max_points` bursts (an even stride through the rows that have a value) are
 * clustered; the others join the cluster of the sampled burst with the
 * smallest mutual-reachability distance among their 10 nearest sampled
 * neighbours, max(core distance, distance), when that is within the
 * cluster's birth distance, and are noise otherwise.
 *
 * `min_cluster_size` (0: max(10, min_cluster_fraction x clustered bursts)) and
 * `min_samples` (0: the minimum cluster size) are HDBSCAN's; `selection` is
 * "leaf" (every density peak: FRET species joined by bleaching or dynamics
 * bridges nest inside one another, and excess of mass keeps only the parent)
 * or "eom".
 *
 * Clusters are then described by per-cluster diagonal Gaussians (mean and
 * width of the members; a missing value has the member fraction missing as
 * its likelihood). HDBSCAN claims the density core of a shot-noise-broadened
 * population and leaves its tails as noise, and population means (alpha,
 * delta, the 1/S vs E line) need the tails: an unclaimed burst joins its most
 * likely cluster when it lies inside that cluster's 99.9% ellipsoid (chi^2
 * over the dimensions it has), three rounds, the widths re-estimated each
 * time; what is left is noise. The normalised Gaussian densities are the
 * assignment probabilities -- HDBSCAN's membership strength ranks a burst within its own
 * cluster and does not sum to one across them. From there the rules of
 * `classify_populations_nd` label, merge and purify, with the density label as
 * the hard label; noise bursts belong to no class and are marked in `noise`.
 * When HDBSCAN finds no cluster the Gaussian mixture is used instead.
 */
PopulationSplit classify_populations_hdbscan(
    const std::vector<double>& x, int n_rows, const std::vector<std::string>& names,
    double donor_only_above = 0.75, double acceptor_only_below = 0.25,
    int min_population = 20, double min_probability = 0.9,
    double min_cluster_fraction = 0.02, int min_cluster_size = 0, int min_samples = 0,
    int max_points = 10000, const std::string& selection = "leaf"
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_MULTIDIM_H
