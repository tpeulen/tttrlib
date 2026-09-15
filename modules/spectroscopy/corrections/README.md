# `spectroscopy/corrections` — Spectral Crosstalk, Background

Correction utilities for single-molecule fluorescence.

## Contents

- **`SpectralCrosstalk.h` / `SpectralCrosstalk.cpp`**:
  - Three-cube ratiometric FRET correction (Hellenkamp et al. 2018)
  - Vectorized batch three-cube correction
  - Ridge-regularized matrix inversion for multi-colour unmixing

- **`BackgroundEstimation.h` / `BackgroundEstimation.cpp`**:
  - Background count-rate estimation from inter-photon time histograms
  - Method-of-moments exponential fit on the tail (background-dominated) region

## Dependencies

- `core`, `util`, `math`
