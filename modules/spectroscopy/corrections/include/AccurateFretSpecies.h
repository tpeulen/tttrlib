// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file AccurateFretSpecies.h
 * \brief Species-specific gamma and the model selection that decides whether
 * it is warranted.
 *
 * A local environment can change the donor or acceptor quantum yield of one
 * FRET species, and gamma = (eta_A phi_A)/(eta_D phi_D) with it. beta is an
 * excitation-flux ratio from which phi_A cancels, so it stays shared; alpha
 * and delta need a reference population of the species and stay pooled.
 *
 * Per FRET population s, with probability-weighted means D = <F_dd>,
 * A = <F_da> (leakage and direct excitation removed) and Y = <F_aa>, the
 * model predicts S_s = (g D + A)/(g D + A + Y/beta) and E_s = A/(A + g D).
 * Observations: S_s = 0.5 (1:1 labelling, which defines beta) and, with a
 * donor lifetime, E_s = E_line(<tau_s>) (static FRET line). Each residual is
 * weighted by the standard error of the population mean plus a model floor
 * `sigma_model` in quadrature. The shared model fits (gamma, beta), the
 * species model (gamma_1..gamma_P, beta); the species model is adopted only
 * when every population has a lifetime observation (else it has more
 * unknowns than equations) and its BIC = chi^2 + k ln(N_obs) is lower.
 */
#ifndef TTTRLIB_ACCURATEFRET_SPECIES_H
#define TTTRLIB_ACCURATEFRET_SPECIES_H

#include <string>
#include <vector>

#include "AccurateFret.h"

namespace tttrlib {

/*!
 * \brief Result of `species_factors`.
 *
 * Per population (parallel vectors of length `n_populations`): effective
 * burst count `n_eff`, model E and S at the adopted factors, mean donor
 * lifetime `tau_d` and E_line (NaN without lifetimes), mean acceptor
 * lifetime `tau_a` (NaN without), and the adopted `gamma`/`sigma_gamma`
 * (the shared value everywhere when the shared model is selected).
 * `gamma_shared`/`beta_shared` and `gamma_species`/`beta_species` are the
 * two fits with their standard errors (from the curvature of chi^2);
 * `selected` is "shared" or "species". `E`/`S` are per burst: FRET bursts
 * are corrected with their populations' gammas weighted by the assignment
 * probabilities, all other bursts with the input factors.
 */
struct SpeciesFactors {
    int n_populations = 0;
    std::vector<double> n_eff, E_pop, S_pop, tau_d, e_line, tau_a;
    std::vector<double> gamma, sigma_gamma;
    double beta = 0.0, sigma_beta = 0.0;
    double gamma_shared = 0.0, sigma_gamma_shared = 0.0;
    double beta_shared = 0.0, sigma_beta_shared = 0.0;
    std::vector<double> gamma_species, sigma_gamma_species;
    double beta_species = 0.0, sigma_beta_species = 0.0;
    double chi2_shared = 0.0, chi2_species = 0.0, bic_shared = 0.0, bic_species = 0.0;
    int n_obs = 0, k_shared = 2, k_species = 0;
    bool identifiable = false;
    std::string selected = "shared";
    std::vector<double> E, S;
};

/*!
 * \brief Fit shared and species-specific gamma and choose by BIC.
 *
 * \param probabilities row-major (n_bursts, n_populations) assignment
 *        probabilities of each burst to each FRET population (rows of
 *        non-FRET bursts are all 0).
 * \param factors alpha, delta and backgrounds used for the signals, and the
 *        starting gamma and beta.
 * \param tau_d, tau_a per-burst lifetimes (ns), or empty.
 * \param line_tau_f, line_efficiency static FRET line, or empty.
 * \param sigma_model model-error floor added to every residual's error.
 */
SpeciesFactors species_factors(
    const std::vector<double>& i_dd, const std::vector<double>& i_da,
    const std::vector<double>& i_aa, const std::vector<double>& probabilities,
    int n_populations, const FretFactors& factors,
    const std::vector<double>& tau_d = std::vector<double>(),
    const std::vector<double>& tau_a = std::vector<double>(),
    const std::vector<double>& line_tau_f = std::vector<double>(),
    const std::vector<double>& line_efficiency = std::vector<double>(),
    double sigma_model = 0.01
);

} // namespace tttrlib

#endif // TTTRLIB_ACCURATEFRET_SPECIES_H
