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
  - Known inherited quirk: the mixture's width update weights squared
    residuals by the *squared* responsibility, `sum((r (x - mu))^2) / N_k`,
    as chisurf's spherical `GaussianMixture` did; the standard EM update is
    `sum(r (x - mu)^2) / N_k`. Kept so the port is exact; soft assignments
    make the widths come out narrower than the ML estimate.
  - Python: `ext/python/AccurateFret.py` wraps the `_afret_*` kernels with
    numpy-in, dict-out signatures (`tttrlib.corrected_es(...)` etc.)

## Dependencies

- `core`, `util`, `math`
