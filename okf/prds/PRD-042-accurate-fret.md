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

## Criteria

1. Kernels above in C++, SWIG-exposed, tests green on the arm64 build.
2. A/B agreement numbers recorded below per stage.
3. Registered in the algorithm registry (PRD-027) with a JSON-schema of
   `auto_calibrate` options.
4. arm64 and Pyodide wheels rebuilt; accurate-FRET check under Node.
5. chisurf callers switched, chisurf copies deleted.

## Results

(filled per stage)
