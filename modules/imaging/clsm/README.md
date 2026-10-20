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

## Frame markers that close the last line

Some scanners raise the frame signal together with the stop of a frame's last
line. PicoQuant marker records are bit fields, so this arrives either as one
record carrying both bits (frame 4 | line stop 2 = routing channel 6) or as the
frame marker followed by the stop within a few ticks. The default routine pairs
every line start with the stop that closes it; a stop with no open line is
ignored, and a line still open at the frame edge is closed by that frame record
or by the stop directly after it. Header-derived PTU markers add the merged
value (`{4, 6}`) to the frame markers, the convention the SP5 routine already
used. The Leica SP5 test file read from its header alone gives
230 x 256 x 256; before, it gave one 7921-line frame.

`test/python/clsm/test_CLSM_frame_marker_with_line_stop.py` covers both record
forms on a synthetic stream and the SP5 header-only read.

## Deferred channel splitting

`split_frames_by_channel()` refreshes the channel layout after constructing its
frame blocks. An image created with `fill=False` and filled later with multiple
channels therefore reports the same channel count, offsets and
`(channels, frames_per_channel, lines, pixels)` shape as an image filled during
construction. Cleared refills reuse the existing blocks rather than duplicating
them again. The public split operation also exposes its new empty layout before
photons are filled.

`test/python/clsm/test_deferred_split_channel_layout.py` compares deferred and
constructor filling, lazy and materialized handles, repeated cleared fills,
explicit splitting, exact per-pixel photon indices and the unchanged unsplit
image layout.

## Python intensity counter width

The Python `CLSMImage.intensity` property and a channel slice's `intensity`
property use `get_intensity_u32()`, returning unsigned 32-bit photon counts.
Pixels with 65,536 or more photons therefore retain their counts rather than
wrapping at the unsigned 16-bit limit. The image keeps its existing
`(frames, lines, pixels)` shape, or `(channels, frames_per_channel, lines, pixels)`
when channel splitting is enabled; selecting a channel returns the frames-first
shape. Explicit `get_intensity()` calls still return the legacy unsigned 16-bit
array, and the native getters and their binary interfaces are unchanged.

`test/python/clsm/test_python_intensity_width.py` exercises ordinary counts and
the 65,535/65,536 boundary, bright pixels, lazy and materialized reconstruction,
multiple frames and channels, and the exact input photon indices. It also
checks that the explicit legacy getter retains its narrower counter behavior.

## Dependencies

- Depends on `core`, `util`, `fcs`.
- nlohmann/json, pocketfft. Not Eigen: the module declared `tttrlib::eigen`
  without including it, and that declaration is gone along with the dependency.
