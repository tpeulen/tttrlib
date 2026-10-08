# `imaging/clsm` — Confocal Laser Scanning Microscopy

Implements spatial reconstruction of 2D/3D images from TTTR marker streams (line sync, frame sync, pixel sync).

## Contents

- **`CLSMImage.h` / `CLSMImage.cpp`**: Top-level CLSM image container (frames, lines, pixels).
- **`CLSMFrame.h` / `CLSMFrame.cpp`**: Frame container for multi-frame CLSM acquisition.
- **`CLSMLine.h` / `CLSMLine.cpp`**: Line container for line scanning markers.
- **`CLSMPixel.h` / `CLSMPixel.cpp`**: Pixel container storing photon indices per pixel.
- **`DecayPhasor.h` / `DecayPhasor.cpp`**: FLIM Phasor transformation per pixel.

## Examples

- `examples/flim/plot_phasor_decay_stack.py` (+ `.ipynb`): phasors of a simulated 18 000-decay stack in one call (`DecayPhasor.compute_phasor_bincounts_batch`), IRF calibration with `DecayPhasor.g/s`, universal-semicircle plot; equals the per-decay `phasor_of_bincounts` digit for digit.

## Simultaneous scan markers

Frame reconstruction includes opening line and pixel markers that share the
frame clock's macro time. A distinct line-stop marker at that same time still
closes the preceding frame. The stop-aware `CLSMImage::get_frame_edges` overload
takes the configured stop marker so the preceding line remains complete and
the next frame does not begin with a zero-duration stop/start pair. Reconstruction
passes this configuration automatically; the original overload and binary
symbol remain available. The C++-only overload does not change the existing
keyword-callable Python helper. Start-only clocks and a shared start/stop
channel keep their existing marker-order behavior.

`test/python/clsm/test_frame_marker_boundaries.py` checks two complete raster
frames without pixel markers, both opening-marker orders, lazy/materialized
counts, exact input photon indices and mean microtimes. It also checks the
legacy positional/keyword-callable signatures and shared or start-only line clocks.

## Dependencies

- Depends on `core`, `util`, `fcs`.
- nlohmann/json, pocketfft. Not Eigen: the module declared `tttrlib::eigen`
  without including it, and that declaration is gone along with the dependency.
