# `spectroscopy/corrections` — Spectral Crosstalk, Background, Accurate FRET

Correction utilities for single-molecule fluorescence.

## Contents

- **`SpectralCrosstalk.h` / `SpectralCrosstalk.cpp`**:
  - Three-cube ratiometric FRET correction (Hellenkamp et al. 2018)
  - Vectorized batch three-cube correction
  - Ridge-regularized matrix inversion for multi-colour unmixing

- **`BackgroundEstimation.h` / `BackgroundEstimation.cpp`**:
  - Background count-rate estimation from inter-photon time histograms
  - Method-of-moments exponential fit on the tail (background-dominated) region

- **`AccurateFret.h`** — `AccurateFretEs.cpp` (two-colour and matrix E/S),
  `AccurateFretGeneral.cpp` (light-path matrices), `AccurateFretDetail.h`
  (private helpers) — accurate single-molecule FRET
  (Hellenkamp et al. 2018), moved here from chisurf so chisurf and ndXplorer share
  one implementation:
  - `apparent_es`, `corrected_es`: per-burst E and S with alpha/beta/gamma/delta
    and backgrounds
  - `corrected_es_matrix`: N chromophores with a coupled donor budget
  - `corrected_es_general`: from the light-path excitation/emission matrices,
    pseudo-inverse or NNLS un-mixing
  - `AccurateFretPopulations.h` — `AccurateFretMixture.cpp` (deterministic 1-D
    Gaussian-mixture EM, BIC selection), `AccurateFretPopulations.cpp`
    (`classify_es_populations`, `split_fret_subpopulations`, alpha from
    donor-only and delta from acceptor-only bursts), `AccurateFretNumerics.cpp`
    (numpy-exact pairwise sums, quantile, linspace, so the A/B against the
    numpy reference holds to the last bits; these sources build with
    `-ffp-contract=off` for the same reason)
  - `AccurateFretCalibrate.h` — `AccurateFretEstimators.cpp`
    (`global_es_correction`: gamma/beta from the 1/S vs E line;
    `beta_from_stoichiometry`; `gamma_from_lifetime` against a tabulated static
    FRET line; `lightpath_correction_factors`) and `AccurateFretAuto.cpp`
    (`auto_calibrate`, also as `auto_calibrate_start/iterate/cancel/finish`
    steps so a caller can report progress and stop between passes; factors are
    clamped to bounds on every write and combined with light-path priors)
  - `AccurateFretUncertainty.h` — `AccurateFretUncertainty.cpp`
    (`efficiency_uncertainty`: gamma/alpha/delta propagated into E;
    `distance_from_efficiency`; `accurate_fret` with per-population summaries)
    and `AccurateFretBootstrap.cpp` (`auto_calibrate_bootstrap`, one class-wise
    resample whose positions can be injected so a caller reproduces a given
    bootstrap exactly, otherwise drawn by the counter-based `Random`;
    `refine_gamma`, the precision-weighted gamma of a labelled FRET sample)
  - `AccurateFretMultiDim.h` — `AccurateFretMixtureNd.cpp` (diagonal-covariance
    Gaussian mixture over standardised columns, missing values marginalised,
    one deterministic start per dimension, BIC selection) and
    `AccurateFretClassifyNd.cpp` (`classify_populations_nd`: donor-only /
    acceptor-only / FRET from every declared dimension -- S, E, donor and
    acceptor lifetimes, anisotropies -- with per-burst FRET sub-population
    probabilities). `auto_calibrate` uses it when `dimensions` is set, derives
    tau_D(0) from the donor-only and tau_A from the acceptor-only class, and
    falls back to the no-linker line E = 1 - tau/tau_D(0) for the lifetime
    gamma when no static FRET line is given
  - `AccurateFretSpecies.h` — `AccurateFretSpecies.cpp` (`species_factors`:
    gamma per FRET population against one shared gamma, both fitted to
    S_s = 0.5 and, with a donor lifetime, E_s = E_line(tau_s); the species
    model is adopted only when identifiable and its BIC is lower; per-burst
    E/S use each burst's population gammas weighted by its assignment
    probabilities). `auto_calibrate` returns it as `species`
  - `AccurateFretRegistry.cpp`: the `corrections/accurate_fret` registry entry
    (operation type `calibration`, JSON schema of the `auto_calibrate` options,
    inputs, outputs, references)
  - Known inherited quirk: the mixture's width update weights squared
    residuals by the *squared* responsibility, `sum((r (x - mu))^2) / N_k`,
    as chisurf's spherical `GaussianMixture` did; the standard EM update is
    `sum(r (x - mu)^2) / N_k`. Kept so the port is exact; soft assignments
    make the widths come out narrower than the ML estimate.
  - Python: `ext/python/AccurateFret.py` wraps the `_afret_*` kernels with
    numpy-in, dict-out signatures (`tttrlib.corrected_es(...)` etc.)

## Dependencies

- `core`, `util`, `math`
