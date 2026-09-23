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
  - Python: `ext/python/AccurateFret.py` wraps the `_afret_*` kernels with
    numpy-in, dict-out signatures (`tttrlib.corrected_es(...)` etc.)

## Dependencies

- `core`, `util`, `math`
