// SPDX-License-Identifier: BSD-3-Clause
// Registry entry of the accurate-FRET calibration (Registry.h, core): declared
// next to the code, registered when this library loads.
#include "Registry.h"

namespace {
const char* const kAccurateFretEntry = R"JSON({
  "name": "accurate_fret",
  "label": "Accurate FRET: automatic calibration",
  "summary": "Determines alpha, beta, gamma and delta from one ALEX/PIE burst measurement, reports accurate E, S and distances with propagated uncertainties, and decides whether FRET species need their own gamma.",
  "description": "Iterated to self-consistency: correct E and S with the current factors, gate donor-only, acceptor-only and FRET bursts with a Gaussian mixture over the stoichiometry (or over every declared dimension: S, E, donor and acceptor lifetimes, anisotropies), take alpha from the donor-only and delta from the acceptor-only bursts, gamma and beta from the 1/S versus E line over the FRET sub-populations or gamma from the donor lifetime and the static FRET line, and combine gamma, alpha and delta with light-path priors by precision weighting. Uncertainties come from a class-wise bootstrap. Finally a shared gamma is compared with one gamma per FRET species (S = 0.5 and, with lifetimes, the static FRET line per species), and the species model is adopted only when it is identifiable and lowers the BIC.",
  "operation_type": "calibration",
  "method": "auto_calibrate",
  "params_schema": {
    "type": "object",
    "properties": {
      "gamma_source": {"type": "string", "title": "gamma source", "enum": ["auto", "es", "lifetime", "combined"], "default": "auto"},
      "n_iterations": {"type": "integer", "title": "self-consistency passes", "minimum": 1, "default": 6},
      "tolerance": {"type": "number", "title": "convergence tolerance", "exclusiveMinimum": 0, "default": 0.001},
      "n_bootstrap": {"type": "integer", "title": "bootstrap resamples", "minimum": 0, "default": 0},
      "seed": {"type": "integer", "title": "bootstrap seed", "minimum": 0, "default": 0},
      "use_priors": {"type": "boolean", "title": "combine with light-path priors", "default": true},
      "assume_one_to_one": {"type": "boolean", "title": "beta from S = 0.5 with one FRET population", "default": true},
      "min_population": {"type": "integer", "title": "smallest population", "minimum": 1, "default": 20},
      "max_fret_populations": {"type": "integer", "title": "most FRET sub-populations", "minimum": 1, "default": 3},
      "donor_only_above": {"type": "number", "title": "donor-only above S", "minimum": 0, "maximum": 1, "default": 0.75},
      "acceptor_only_below": {"type": "number", "title": "acceptor-only below S", "minimum": 0, "maximum": 1, "default": 0.25},
      "dimensions": {"type": "array", "title": "gating dimensions", "items": {"type": "string", "enum": ["S", "E", "tau_d", "tau_a", "r_d", "r_a"]}, "default": []},
      "donor_lifetime": {"type": "number", "title": "donor-only lifetime (ns), <= 0 to estimate", "default": -1.0},
      "min_probability": {"type": "number", "title": "reference-class purity", "minimum": 0, "maximum": 1, "default": 0.9},
      "max_components_nd": {"type": "integer", "title": "most mixture components (multidimensional)", "minimum": 1, "default": 6},
      "species_factors": {"type": "boolean", "title": "test species-specific gamma", "default": true},
      "sigma_model": {"type": "number", "title": "model error floor", "minimum": 0, "default": 0.01}
    }
  },
  "inputs": {
    "required": ["i_dd", "i_da"],
    "optional": ["i_aa", "tau_d", "tau_a", "r_d", "r_a"]
  },
  "outputs": {
    "columns": ["E", "S"],
    "scalars": ["gamma", "alpha", "beta", "delta", "sigma_gamma", "sigma_alpha", "sigma_beta", "sigma_delta", "tau_d0", "tau_a"]
  },
  "row_grain": "burst",
  "references": [
    {"type": "journal", "authors": "Hellenkamp, B. et al.", "title": "Precision and accuracy of single-molecule FRET measurements -- a multi-laboratory benchmark study", "journal": "Nat Methods", "year": 2018, "volume": "15", "pages": "669-676"},
    {"type": "journal", "authors": "Lee, N. K., Kapanidis, A. N., Wang, Y., Michalet, X., Mukhopadhyay, J., Ebright, R. H., Weiss, S.", "title": "Accurate FRET measurements within single diffusing biomolecules using alternating-laser excitation", "journal": "Biophys J", "year": 2005, "volume": "88", "pages": "2939-2953"},
    {"type": "journal", "authors": "Kalinin, S., Valeri, A., Antonik, M., Felekyan, S., Seidel, C. A. M.", "title": "Detection of structural dynamics by FRET: a photon distribution and fluorescence lifetime analysis of systems with multiple states", "journal": "J Phys Chem B", "year": 2010, "volume": "114", "pages": "7983-7995"},
    {"type": "journal", "authors": "Sisamakis, E., Valeri, A., Kalinin, S., Rothwell, P. J., Seidel, C. A. M.", "title": "Accurate single-molecule FRET studies using multiparameter fluorescence detection", "journal": "Methods Enzymol", "year": 2010, "volume": "475", "pages": "455-514"}
  ],
  "api": [
    "auto_calibrate", "accurate_fret", "corrected_es", "apparent_es", "corrected_es_matrix",
    "corrected_es_general", "classify_es_populations", "classify_populations_nd",
    "gaussian_mixture_1d", "gaussian_mixture_nd", "best_gaussian_mixture_1d",
    "best_gaussian_mixture_nd", "split_fret_subpopulations", "global_es_correction",
    "gamma_from_lifetime", "beta_from_stoichiometry", "leakage_from_donor_only",
    "direct_excitation_from_acceptor_only", "efficiency_uncertainty",
    "distance_from_efficiency", "refine_gamma", "species_factors",
    "lightpath_correction_factors"
  ],
  "can_replay": true
})JSON";
bool register_accurate_fret_entries() {
    tttrlib::register_algorithm_json("corrections", "accurate_fret", kAccurateFretEntry);
    return true;
}
const bool kAccurateFretRegistered = register_accurate_fret_entries();
}  // namespace
