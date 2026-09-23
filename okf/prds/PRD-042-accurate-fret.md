# PRD-042 — Accurate FRET in tttrlib: corrected E/S, auto-calibration, uncertainties

> **PRD #:** 042 · **Status:** 🟡 In Progress · **Created:** 2026-09-23 · **Owner:** tpeulen
> **Related:** PRD-027 (algorithm registry), PRD-029 (drop-in verification),
> PRD-041 (Pyodide wheel — ndXplorer in the browser needs these kernels too)

## Summary

The accurate-FRET algorithms live in chisurf Python today
(`core/fluorescence/burst/es.py`, `fret/accurate.py`, `fret/calibration.py`).
ndXplorer needs the same maths (Qt, emtk, desktop and browser). The placement
rule "photons/curves → tttrlib" puts them here: one C++ implementation in
`modules/spectroscopy/corrections`, SWIG-exposed, called by chisurf and ndXplorer.

## Boundary

**Moves to tttrlib (C++):**
- corrected E/S: `apparent_es`, `corrected_es` (α/β/γ/δ + backgrounds),
  `corrected_es_matrix` (N chromophores, coupled donor budget),
  `corrected_es_general` (light-path excitation/emission matrices; naive
  pseudo-inverse or NNLS un-mixing);
- the S population mixture: the 1-D Gaussian-mixture EM, BIC selection,
  `classify_es_populations`, `split_fret_subpopulations`;
- reference estimators: `leakage_from_donor_only` (α),
  `direct_excitation_from_acceptor_only` (δ), `global_es_correction`
  (γ/β from 1/S vs E), `beta_from_stoichiometry`, `gamma_from_lifetime`
  (static FRET line passed as a tabulated `(tau_f, E)` polyline), the scalar
  light-path factor algebra, the Gaussian prior/data combination;
- `auto_calibrate` (the self-consistent iteration, bootstrap, priors);
- `efficiency_uncertainty`, `distance_from_efficiency`, `accurate_fret` with
  per-population summaries.

**Stays in chisurf:** `CalibrationParameters` (fitting parameters, priors),
light-path payload parsing (`matrix_from_payload`, label lookup),
`set_priors_from_lightpath`, `CalibrationFit` registration, setup/ndx constant
conversion, FRET-line *construction* (`fret/lines.py`), the `AutoCalibration`
report text. chisurf calls tttrlib and keeps no second copy.

## API (Python-facing; C++ mirrors it with result structs)

Arrays are numpy float64, one entry per burst. `i_aa=None` means no
acceptor-excitation channel.

```
tttrlib.apparent_es(i_dd, i_da, i_aa=None) -> {"E", "S"}
tttrlib.corrected_es(i_dd, i_da, i_aa=None, *, gamma, alpha, beta, delta,
                     bg_dd, bg_da, bg_aa) -> {"E", "S", "fc"}
tttrlib.corrected_es_matrix(intensity, gamma, alpha, delta=None,
                            background=None, pairs=None) -> {(i, j): {"E", "fc"}}
tttrlib.corrected_es_general(intensity, excitation, emission, *, background,
                             pairs, unmix="naive"|"stable", ridge) -> {(l, k): {"E", "fc"}}
tttrlib.gaussian_mixture_1d(x, n_components, ...) -> dict
tttrlib.classify_es_populations(S, E=None, ...) -> dict (masks, labels, thresholds)
tttrlib.auto_calibrate(columns, constants=None, options=None) -> dict
tttrlib.accurate_fret(i_dd, i_da, i_aa=None, *, factors, uncertainties,
                      tau_f, line, labels) -> dict
```

C++: `tttrlib::FretFactors {gamma, alpha, beta, delta, bg_dd, bg_da, bg_aa, r0}`,
`EsResult`, `MixtureResult`, `PopulationSplit`, `AutoCalibrateOptions`,
`AutoCalibrationResult`; the SWIG names carry an `_afret_` prefix and a
`%pythoncode` layer turns the structs into the dicts above.

## Bootstrap randomness

The bootstrap resampling indices are **injectable**: `auto_calibrate` accepts
precomputed per-class index arrays, so an A/B test passes the same indices to
both implementations. Without them tttrlib draws with its own `Random`. No
bit-exact numpy PCG64 is written.

## A/B plan

Every stage ships `test/python/corrections/test_accurate_fret_*.py` that runs
the tttrlib kernel and the chisurf reference on the same synthetic arrays:
closed forms (E/S, estimators, distance, uncertainty) to 1e-12; the EM mixture
to 1e-10 (summation order differs from BLAS); labels and masks exactly.
`auto_calibrate` is also compared on the cal1 ALEX/PIE file
(`sm/cal1/001_60g_25r_cal1_cy3b_8_18_33bp_atto647n_alex.pto`, burst columns via
`ndxplorer.io.loading.read`). The reference is a numpy transcription of the
chisurf code kept beside the tests (`test/python/corrections/afret_reference/`),
so the A/B survives the deletion of chisurf's copy; while chisurf still has
it, a test also checks the transcription against chisurf itself.

## Extension (tpeulen, 2026-09-23): all dimensions, species-specific factors

Built **after** the A/B-pinned port (stages 1-4), so the chisurf behaviour is
fixed first. Validated against ground truth on synthetic data; the A/B only
covers what chisurf already has (S-only gating, one global factor set).

**Multidimensional populations.** Gating and estimation use every per-burst
dimension present, declared rather than hardcoded: the caller passes a
`dimensions` list naming columns from a declared vocabulary (`S`, `E`,
`tau_d` donor lifetime, `tau_a` acceptor lifetime, `r_d`/`r_a` anisotropies,
and the raw ALEX/PIE channel counts). The mixture becomes a diagonal-covariance
Gaussian mixture over the standardised declared columns; components are still
*labelled* by their S centre (donor-only S ≈ 1, acceptor-only S ≈ 0) when S is
present, otherwise by E and tau_d (donor-only: E ≈ 0 and tau_d ≈ tau_D(0)).
Missing columns degrade to the S-only path, which stays bit-compatible with
the port. From the reference classes come tau_D(0) (donor-only mean lifetime,
replacing the "longest observed lifetime" fallback) and tau_A (acceptor-only),
reported as an acceptor-photophysics check.

**Lifetime as independent information for gamma.** For a static population
the lifetime fixes the efficiency, E_tau = 1 - tau_D(A)/tau_D(0) (no linker
dynamics) or the static FRET line E_line(tau_f) including linker broadening
(Kalinin et al., J. Phys. Chem. B 114, 7983, 2010; Sisamakis et al., Methods
Enzymol. 475, 455, 2010; Barth et al., J. Chem. Phys. 156, 141501, 2022).
Intensity E(gamma) = F_DA / (F_DA + gamma F_DD) must equal it, which gives
gamma per population. Combined with the E-S route (Lee et al., Biophys. J.
88, 2939, 2005; Hellenkamp et al., Nat. Methods 15, 669, 2018) gamma is
constrained by both. Caveat, stated in the result: a dynamic population lies
off the static line (towards longer tau_f at the same E), which the lifetime
route would read as a smaller gamma; populations flagged as off-line by more
than their uncertainty are excluded from the lifetime gamma.

**Species-specific factors.** A local environment can change the donor or
acceptor quantum yield of one species, and with it gamma (gamma =
eta_A phi_A / eta_D phi_D). beta is an excitation-flux ratio and phi_A cancels
from S, so beta stays shared unless requested. alpha and delta need a
reference population of that species and are always pooled; the result says
so. Model: per FRET population s the observations are its mean corrected
S (must equal 0.5 for 1:1 labelling, which defines beta) and, when lifetimes
exist, E(gamma_s) - E_line(tau_s), each weighted by its standard error. The
shared model fits (gamma, beta); the species model fits (gamma_1..gamma_n,
beta). The species model is adopted only when it lowers the BIC
(chi² + k ln N_obs); the result reports both BICs and the decision. Without
lifetimes the species model is not identifiable (n + 1 unknowns, n
equations) and is never selected. Per-burst corrected E/S use the burst's
population factors weighted by the mixture's assignment probabilities.

**Result additions:** `factors` (global), `population_factors` (per
population: gamma, beta, sigma, `pooled` flags), `model_selection` (`bic_shared`,
`bic_species`, `selected`), `tau_d0`, `tau_a`, the declared `dimensions` used.

**Extension tests:** synthetic bursts with known species-specific gamma and
known lifetimes (recovered within uncertainty); a shared-gamma dataset (no
species factors invented, BIC keeps the shared model); the cal1 .pto.

## Criteria

1. Kernels above in C++, SWIG-exposed, tests green on the arm64 build.
2. A/B agreement numbers recorded below per stage.
3. Registered in the algorithm registry (PRD-027) with a JSON-schema of
   `auto_calibrate` options.
4. arm64 and Pyodide wheels rebuilt; accurate-FRET check under Node.
5. chisurf callers switched, chisurf copies deleted.
6. Extension: multidimensional gating and species-specific gamma pass the
   ground-truth tests above.

## Results

- **Stage 1, E/S (2026-09-23):** `apparent_es`, `corrected_es`,
  `corrected_es_matrix`, `corrected_es_general` (naive + NNLS). A/B max abs
  difference vs the transcription: corrected_es 2.8e-14 (on fc ~ 1e2, i.e.
  last bit), matrix 1.1e-16, general naive 8.9e-16, general NNLS 3.1e-15.
  The transcription equals chisurf to 1e-14 (chisurf's three-cube step ran in
  C++, FMA). `test_accurate_fret_es.py`, 30 cases.
- **Stage 2, populations (2026-09-23):** `gaussian_mixture_1d`,
  `best_gaussian_mixture_1d`, `classify_es_populations`,
  `split_fret_subpopulations`, `leakage_from_donor_only`,
  `direct_excitation_from_acceptor_only`. numpy-exact pairwise summation
  and `-ffp-contract=off`. Over 6 seeds x k = 1..4: mixture parameters and
  responsibilities max abs diff 3.6e-14, 0 label/mask mismatches, identical
  EM iteration counts; estimators to 1e-12. Found and kept (documented in the
  module README): chisurf's spherical EM weights squared residuals by the
  squared responsibility. `test_accurate_fret_populations.py`, 38 cases.
- **Stage 3, auto_calibrate (2026-09-23):** `global_es_correction`,
  `beta_from_stoichiometry`, `gamma_from_lifetime` (line as a tabulated
  polyline, numpy.interp semantics), `lightpath_correction_factors`,
  `auto_calibrate` (+ start/iterate/cancel/finish steps, progress callback in
  Python). 16 synthetic scenarios (priors, lifetime and combined gamma, one
  FRET population, no ALEX, one pass, out-of-bounds start): factors to 1e-10,
  masks/labels/messages/iterations identical. cal1 .pto (44 270 bursts; 3343
  donor-only, 10 261 acceptor-only, 26 945 FRET in 3 populations): alpha
  0.157401645687572 and delta 0.067328008822786 bit-identical, gamma 0.825635
  (diff 1.8e-15), beta 0.967539 (diff 1.1e-15), 6 passes, converged, splits
  identical. The chisurf transcription check passes against chisurf's own
  auto_calibrate. `test_accurate_fret_calibrate.py`, 25 cases.
- **Stage 4, uncertainties (2026-09-23):** `efficiency_uncertainty`,
  `distance_from_efficiency`, `accurate_fret` (+ population summaries,
  numpy-exact nanmean/nanstd), `auto_calibrate_bootstrap` (class-wise
  resampling, positions injectable via `bootstrap_indices`, else
  `Random::deterministic(seed, counter)`), `refine_gamma`. No numpy PCG64 was
  written: the A/B hands tttrlib chisurf's own `default_rng(seed).integers`
  draws. Per-burst E, sigma_E, distance and its sigma to rtol 1e-12; population
  summaries max abs diff 3.6e-14; bootstrap sigmas max rel diff 2.1e-14 over 4
  seeds x 50 resamples. cal1 with 50 resamples: alpha 0.15740 +- 0.00223,
  delta 0.06733 +- 0.00057, gamma 0.82564 +- 0.00712, beta 0.96754 +- 0.00428,
  sigmas equal to the reference to 1e-13 relative; FRET populations at
  E = 0.360 (R = 57.24 +- 0.12 A) and E = 0.802 (R = 41.21 +- 0.06 A).
  `test_accurate_fret_uncertainty.py`, 16 cases.
- **Extension E1, multidimensional gating (2026-09-23):**
  `gaussian_mixture_nd`, `best_gaussian_mixture_nd`, `classify_populations_nd`,
  `auto_calibrate(options={"dimensions": [...]})`, declared vocabulary
  `tttrlib.AFRET_DIMENSIONS` (S, E, tau_d, tau_a, r_d, r_a). Missing values
  are marginalised (donor-only bursts have no acceptor lifetime); corrected
  S/E outside [-0.5, 1.5] count as missing (acceptor-only E is noise over
  noise and otherwise widened one component over the axis); FRET components
  within one width of a larger one in every dimension are the same species
  (shot noise skews E) and are merged; for the 1/S vs E fit, sub-populations
  closer than 0.05 in E are pooled (a lifetime-only split carries no gamma
  information on that line). tau_D(0) from the donor-only class; without a
  line the no-linker line E = 1 - tau/tau_D(0). Ground truth (synthetic MFD
  with lifetimes): reference classes > 99% pure, 2 FRET species found, tau_D(0)
  and tau_A within 0.05 ns, alpha/delta within 0.005, gamma/beta within 5%;
  two species at equal E but different donor lifetime are split (S-only finds
  one); lifetime gamma of a single species within 5%. cal1 with
  `dimensions=["S","E"]`: 24.6 s for 44 270 bursts (S-only: 2.4 s), 4 FRET
  populations, gamma 0.731 vs 0.826 S-only -- not validated against anything
  on real data yet. `test_accurate_fret_multidim.py`, 6 cases.
- **Extension E2, species-specific gamma (2026-09-23):** `species_factors`
  (C++), run by `auto_calibrate` on the final FRET populations and returned
  as `result["species"]`. Implemented as specified above with one change:
  beta is always shared (phi_A cancels from S; a per-species beta would be
  exactly determined by S_s = 0.5 plus the lifetime and adds nothing to test),
  alpha/delta always pooled; FRET components of the multidimensional gating
  closer than two pooled widths are one species. Ground truth: gamma_s =
  (0.6, 1.2) recovered within 8% (e.g. 0.602 +- 0.025, 1.207 +- 0.056;
  BIC 4.2 species vs 125.4 shared), per-burst E of each species within 0.02;
  gamma = 1.0 for both species keeps the shared model (BIC 2.8 vs 4.2);
  without lifetimes not identifiable, never selected; cal1 (no lifetimes)
  keeps the shared factors. Acceptor lifetime per population is reported
  (tracks phi_A). `test_accurate_fret_species.py`, 12 cases.

- **Stage 5, registry (2026-09-23):** `corrections/accurate_fret`
  (operation type `calibration`, replayable, so it also reaches the
  `operation` category): JSON schema of every `auto_calibrate` option (a test
  pins schema keys and defaults to `AutoCalibrateOptions` and the Python option
  list), inputs/outputs, the dimension vocabulary, four references, and the
  `api` list (every name resolves in Python). The result structs are declared
  plumbing in `test_registry_completeness.py`. Two failures in the registry
  suites predate this branch (`can_compress`/`can_decompress` from ptolib,
  `Correlator` missing from the API index of the split build).

- **Stage 6, wheels (2026-09-23):** arm64 wheel built with the given `pip
  wheel` command and installed with `pip install --force-reinstall` (no dylib
  copied); the corrections suite passes against the installed wheel;
  `import IMP.bff` works against it. Pyodide wheel from
  `tools/pyodide/build_wheel.sh` (3.15 MB); `smoke_test.mjs` gained an
  accurate-FRET check: synthetic ALEX bursts drawn by numpy's PCG64 give the
  native arm64 factors and bootstrap sigmas to rtol 1e-9 under Node.

Result structure (`result["species"]`), for vector-valued constants in ndX:

```
{"labels": [0, 1, ...], "names": ["FRET 1", ...],
 "populations": [{"label", "name", "n", "E", "S", "tau_d", "E_line", "tau_a"}, ...],
 "factors": {"gamma"|"beta"|"alpha"|"delta":
             {"global": float, "values": [per population], "sigma": [per population],
              "pooled": bool}},
 "assignment": ndarray (n_bursts, n_populations)  # 0 rows outside FRET
 "label": ndarray (n_bursts,)                      # hard label, -1 outside FRET
 "E", "S": ndarray (n_bursts,)                     # corrected with population factors
 "model_selection": {"selected": "shared"|"species", "identifiable", "criterion",
                     "bic_shared", "bic_species", "chi2_shared", "chi2_species",
                     "n_obs", "k_shared", "k_species", "gamma_shared",
                     "sigma_gamma_shared", "beta_shared", "gamma_species",
                     "sigma_gamma_species", "beta_species"}}
```
