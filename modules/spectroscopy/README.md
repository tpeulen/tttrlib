# `spectroscopy` — Single-Molecule Spectroscopy Modules

Top-level module directory for single-molecule spectroscopy submodules.

## Submodules

- **`burst`**: Photon burst search and feature extraction.
- **`decay`**: Fluorescence decay analysis and iterative reconvolution fitting algorithms.
- **`fcs`**: Fluorescence correlation spectroscopy (FCS) autocorrelation and cross-correlation curves.
- **`hmm`**: Hidden Markov Model analysis of time traces and burst data.
- **`pda`**: Photon Distribution Analysis for single-molecule FRET histograms.
- **`kinetics`**: Continuous-time Markov chain utilities and Gopich-Szabo photon-by-photon likelihood.
- **`fluctuation`**: Photon counting histogram (PCH) and FIDA distributions.
- **`corrections`**: Spectral crosstalk correction, background estimation, maximum entropy inversion.
- **`burstpipeline`**: The fused burst pipeline: search, selection, burst table, CSR per-burst decays, strict background and per-burst fits in a few native calls, with zero-copy result views (depends on `burst` and `decay`).
