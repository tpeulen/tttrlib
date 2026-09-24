// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file AccurateFret.h
 * \brief Accurate single-molecule FRET: corrected efficiency and
 * stoichiometry per burst.
 *
 * Per-burst photon counts under donor and acceptor excitation are turned into
 * the apparent and the fully corrected FRET efficiency E and stoichiometry S
 * (Hellenkamp et al., Nat. Methods 15, 669, 2018). Channel names follow the
 * two-colour convention: `i_dd` is donor emission under donor excitation,
 * `i_da` acceptor emission under donor excitation (the FRET channel) and
 * `i_aa` acceptor emission under acceptor excitation (ALEX/PIE).
 *
 * The correction factors are `alpha` (donor leakage into the acceptor
 * channel), `delta` (direct acceptor excitation by the donor laser, relative
 * to the acceptor-excitation channel), `gamma` (detection and quantum-yield
 * ratio) and `beta` (excitation-flux ratio, which only enters S):
 *
 *     F_dd = i_dd - bg_dd,  F_aa = i_aa - bg_aa
 *     F_da = (i_da - bg_da) - alpha F_dd - delta F_aa
 *     E = F_da / (F_da + gamma F_dd)
 *     S = (gamma F_dd + F_da) / (gamma F_dd + F_da + F_aa / beta)
 *
 * The N-chromophore generalisations take the intensity matrix I[l, m]
 * (signal of detection channel m under laser l) with bursts as the trailing,
 * fastest-varying axis, flattened row-major.
 */
#ifndef TTTRLIB_ACCURATEFRET_H
#define TTTRLIB_ACCURATEFRET_H

#include <string>
#include <vector>

namespace tttrlib {

/*!
 * \brief The correction factors, backgrounds and Förster radius of one
 * two-colour calibration.
 *
 * Defaults are the uncorrected calibration: gamma = beta = 1, alpha = delta
 * = 0, no background, and R0 = 52 Å.
 */
struct FretFactors {
    double gamma = 1.0;   ///< detection / quantum-yield ratio
    double alpha = 0.0;   ///< donor leakage into the acceptor channel
    double beta = 1.0;    ///< excitation-flux ratio (stoichiometry only)
    double delta = 0.0;   ///< direct acceptor excitation, relative to I_AA
    double bg_dd = 0.0;   ///< background of I_DD per burst
    double bg_da = 0.0;   ///< background of I_DA per burst
    double bg_aa = 0.0;   ///< background of I_AA per burst
    double r0 = 52.0;     ///< Förster radius in Å
};

/*!
 * \brief Per-burst efficiency, stoichiometry and sensitised emission.
 *
 * `E`, `S` and `fc` have one entry per burst. `S` is empty and `has_s` is
 * false when no acceptor-excitation channel was given. `fc` is the corrected
 * sensitised emission F_da (empty for `apparent_es`).
 */
struct EsResult {
    std::vector<double> E;
    std::vector<double> S;
    std::vector<double> fc;
    bool has_s = false;
};

/*!
 * \brief Pairwise corrected efficiencies of an N-chromophore measurement.
 *
 * Pair p is donor `donor[p]` to acceptor `acceptor[p]` (0-based). `E` and
 * `fc` are row-major `(n_pairs, n_bursts)`: the value of pair p for burst b
 * is at `p * n_bursts + b`. Pairs are ordered by donor, in the order each
 * donor first appears in the requested pair list, then by the requested
 * acceptor order.
 */
struct PairEsResult {
    std::vector<int> donor;
    std::vector<int> acceptor;
    int n_bursts = 0;
    std::vector<double> E;
    std::vector<double> fc;
};

/*!
 * \brief Apparent (uncorrected) proximity ratio and raw stoichiometry.
 *
 * E = i_da / (i_dd + i_da) and S = (i_dd + i_da) / (i_dd + i_da + i_aa),
 * each 0 where its denominator is exactly 0.
 *
 * \param i_dd, i_da per-burst counts under donor excitation.
 * \param i_aa per-burst acceptor counts under acceptor excitation; pass an
 *        empty vector when there is none (then `S` is empty, `has_s` false).
 * \throws std::invalid_argument when the lengths differ.
 */
EsResult apparent_es(
    const std::vector<double>& i_dd,
    const std::vector<double>& i_da,
    const std::vector<double>& i_aa = std::vector<double>()
);

/*!
 * \brief Fully corrected per-burst E and S (Hellenkamp 2018).
 *
 * Applies the formulas in the file description. E is 0 where
 * F_da + gamma F_dd <= 0 (an over-subtracted burst), S is 0 where its
 * denominator is exactly 0. Without an acceptor-excitation channel F_aa is
 * taken as 0, so `delta` has no effect and no S is returned.
 *
 * \param i_dd, i_da per-burst counts under donor excitation.
 * \param i_aa per-burst counts under acceptor excitation, or empty.
 * \param factors correction factors and backgrounds (`r0` unused).
 * \throws std::invalid_argument when the lengths differ.
 */
EsResult corrected_es(
    const std::vector<double>& i_dd,
    const std::vector<double>& i_da,
    const std::vector<double>& i_aa,
    const FretFactors& factors
);

/*!
 * \brief Pairwise corrected efficiencies with a coupled donor budget.
 *
 * For donor i and each of its requested acceptors j
 *
 *     F_ij = (I_ij - Bg_ij) - alpha_ij (I_ii - Bg_ii) - delta_ij (I_jj - Bg_jj)
 *     E_ij = (F_ij / gamma_ij) / (F_ii + sum_k F_ik / gamma_ik)
 *
 * so a donor quenched by several acceptors yields each pairwise E exactly; a
 * single acceptor reduces to `corrected_es`. E is 0 where the budget is 0.
 *
 * \param intensity row-major (n, n, n_bursts): I[i, j, b].
 * \param n number of chromophores.
 * \param n_bursts number of bursts (trailing axis).
 * \param gamma, alpha row-major (n, n) factor matrices.
 * \param delta row-major (n, n), or empty for zeros.
 * \param background row-major (n, n), or empty for zeros.
 * \param pairs flat (donor, acceptor) index pairs; empty means every i < j.
 * \throws std::invalid_argument on a size mismatch or an index out of range.
 */
PairEsResult corrected_es_matrix(
    const std::vector<double>& intensity, int n, int n_bursts,
    const std::vector<double>& gamma,
    const std::vector<double>& alpha,
    const std::vector<double>& delta,
    const std::vector<double>& background,
    const std::vector<int>& pairs
);

/*!
 * \brief Pairwise corrected efficiencies from the light-path crosstalk
 * matrices, for any number of chromophores.
 *
 * `excitation[l, k]` is the rate at which laser l excites chromophore k;
 * `emission[k, m]` the detected brightness of chromophore k in channel m.
 * Three steps:
 *
 * 1. un-mix: e[l, :] solves I[l, :] = e[l, :] emission for every laser and
 *    burst. `unmix = "naive"` (aliases "pinv", "linear") uses the
 *    minimum-norm pseudo-inverse, or the Tikhonov solution
 *    (emission emission^T + ridge I)^-1 emission I[l, :] when ridge > 0;
 *    `"stable"` (aliases "nnls", "nonneg") solves the non-negative least
 *    squares problem per burst, with the ridge as an augmented system;
 * 2. subtract direct excitation: F[l, k] = e[l, k] - (x[l, k] / x[k, k]) e[k, k]
 *    (0 when x[k, k] = 0);
 * 3. coupled budget: E[l, k] = F[l, k] / (e[l, l] + sum_a F[l, a]), 0 where
 *    the budget is 0.
 *
 * For two colours with emission = [[1, alpha], [0, gamma]] and excitation =
 * [[1, delta], [0, 1]] this equals `corrected_es`.
 *
 * \param intensity row-major (n_lasers, n_detectors, n_bursts).
 * \param excitation row-major (n_lasers, n_chromophores).
 * \param emission row-major (n_chromophores, n_detectors).
 * \param background row-major (n_lasers, n_detectors), or empty.
 * \param pairs flat (laser/donor, acceptor) pairs; empty means l < k.
 * \param unmix "naive" or "stable" (or an alias).
 * \param ridge Tikhonov strength (>= 0).
 * \throws std::invalid_argument on a size mismatch, an index out of range or
 *         an unknown `unmix`.
 */
PairEsResult corrected_es_general(
    const std::vector<double>& intensity,
    int n_lasers, int n_detectors, int n_bursts,
    const std::vector<double>& excitation,
    const std::vector<double>& emission, int n_chromophores,
    const std::vector<double>& background,
    const std::vector<int>& pairs,
    const std::string& unmix = "naive",
    double ridge = 0.0
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_H
