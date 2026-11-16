# `core` — Core TTTR Data Structures and Operations

This module provides the central `TTTR` photon container class, channel routing, microtime calibration, and event filters.

## Contents

The photon-stream data model, the histograms every analysis bins with, the
columnar `DataStore`, and **the registry**.

- **`TTTR.h` / `TTTR.cpp`** — the photon container: macro times, micro times,
  routing channels, event types; reading and writing every container through
  the `io_*` modules; selections, ranges and the burst-search entry points
  (defined in [`spectroscopy/burst`](../spectroscopy/burst)).
- **`TTTRHeader.h` / `TTTRHeader.cpp`** — the header: tags, resolutions,
  per-vendor setup metadata, JSON round trip.
- **`TTTRRecordReader.h`** — the per-record-type decoders (header-only,
  compile-time stride), shared by the format modules.
- **`TTTRStream.h` / `TTTRStream.cpp`** — decoding a buffer with a carried
  state, for streaming and partial reads.
- **`RecordStreamWriter.h` / `RecordStreamWriter.cpp`** — writing records back
  out in a container's own encoding.
- **`TTTRMask.h` / `TTTRMask.cpp`** — a bit mask over event indices.
- **`TTTRSelection.h` / `TTTRSelection.cpp`** — index selections by channel,
  micro-time gate, event type and time window.
- **`TTTRRange.h` / `TTTRRange.cpp`** — the (start, stop) form a burst table
  is written in.
- **`Channel.h` / `Channel.cpp`** — a *named* detector: routing channel plus
  micro-time gate, which is what CLSM and burst analyses take.
- **`Histogram.h` / `Histogram.cpp`, `HistogramAxis.h` / `HistogramAxis.cpp`,
  `HistogramNd.h`** — 1-D, N-D, weighted, linear and log axes, threaded fill.
- **`DataStore.h` / `DataStore.cpp`** — the columnar table: dtype-preserving
  columns, dictionary-encoded strings, bit-packed masks, zero-copy NumPy/R
  views. The files that *save* one are [`io/store`](../io/store),
  [`io/hdf5`](../io/hdf5), [`io/csv`](../io/csv) and [`io/pto`](../io/pto).
- **`DataStore.h`** — re-exports the DataStore, Column, BitMask, registry and the expression engine from ptolib (`thirdparty/ptolib/include/ptolib/ptolib.h`, shared with IMP.bff) under `tttrlib::data`, and adds the `HistogramNd` fills over columns. The core links `ptolib::ptolib`, which compiles the table, expression engine, `.dstore` and PTO storage, and codec sources separately. `src/DataStore.cpp` only includes the public header. Codec providers are selected through `PTOLIB_<CODEC>_PROVIDER` (`bundled`, `system`, or `disabled`); `PTOLIB_DECODE_ONLY_CODECS` accepts a semicolon-separated list for reader-only codecs.
- **`MicrotimeLinearization.h` / `MicrotimeLinearization.cpp`** — per-channel
  DNL correction (look-up table) and micro-time shifts.
- **`FileCheck.h` / `FileCheck.cpp`** — what a file is: the content sniffers
  and `inferTTTRFileType`.
- **`Registry.h` / `Registry.cpp`** — **the registry**. One table
  (`register_algorithm` / `register_algorithm_json`) that every algorithm, fit
  model, objective, prior, correlation method and pipeline operation registers
  itself in, next to its own code, when its library loads; `registry_json()`
  assembles the categories from what registered, plus the file-format,
  table-format and plugin catalogs. See [`modules/README.md`](../README.md).
- **`RegistryCore.h`** — the registry's mechanism, header-only and std-only:
  descriptors, the table they register in, the entry shape. `Registry.h`
  above is one user of it; imp.bff vendors this same header, pinned, for its
  own registry, so both read with the same code.

## ALEX phase folding

`TTTR::alex_to_microtime(period, shift)` replaces micro times with
`(macrotime - shift) modulo period`, using circular, nonnegative phases even
when the shift exceeds an early macrotime. It accepts the full unsigned
64-bit macrotime domain and signed integer shifts. Macro times, routing
channels, event types and header metadata remain unchanged.

The period must be **1 through 65536**: every phase must fit the existing
unsigned 16-bit microtime column. Zero or larger periods raise before any
event is changed, including on empty streams. Storage capacity is distinct
from file capacity: HydraHarp/Generic T3 uses 15 microtime bits (period at most
32768), PicoHarp T3 and SPC-130/SPC-600_4096/QC use 12 bits (at most 4096), and
SPC-600_256 uses 8 bits (at most 256). T2, SM and CZ records have no microtime
field. Callers exporting an ALEX phase must validate the target record format;
folding does not widen its fields or update physical-clock metadata.

The independent integer oracle and atomic rejection checks live in
`test/python/tttr/test_alex_microtime_folding.py`.

The bulk macrotime getter retains the complete unsigned 64-bit range too.
Cached display offsets preserve the existing lower-bound clamp at zero;
a positive offset that exceeds unsigned capacity raises before allocating
output, instead of overflowing. Neither a getter nor its cached display
offset modifies stored event times.

## Folding the alternation out of the macro-time clock

`AlternationFold.h` (header-only, std-only) and `TTTR::get_folded_macro_times`
map every µs-ALEX alternation period onto one excitation slot and overlay the
slots, so that analyses of photon arrival times do not see the laser
alternation as a property of the molecule. A photon with alternation phase
`phi` (the micro time after `alex_to_microtime`) at macro time `t` folds to

```
psi = (phi - s_0) mod P,   k = slot of psi,   c = t - psi
t'  = floor(c * L / P) + (psi - (s_k - s_0)) + L
```

with slot starts `s_k` and the longest slot length `L`. One folded tick is
`P / L` macro ticks of real time (`get_tick_scale`, 2 for two equal slots).
`AlternationFold::from_windows(period, [start, stop, ...])` places the slots
around the excitation windows: equal slots when they fit, otherwise one slot
boundary in the middle of every gap. It also centre-aligns the windows on a
common window of the shortest window's length (`in_common_window`,
`get_common_window_length`), because gates of unequal length or offset leave
part of every folded period with one excitation gated, which a photon HMM fits
as a state. `TTTR::get_alternation_period()` reads the
ALEX-conversion header contract (micro-time resolution equal to the macro-time
resolution, the period as number of micro-time channels) and returns 0 for
TCSPC/PIE data; without an explicit fold `get_folded_macro_times` uses two
equal slots of that period.

The folded clock is meant for photon-by-photon HMMs, BVA/SVA time windows,
burst-wise PDA windows and correlations at lags below the period. Burst
search, burst durations and count rates stay on the real clock. The folded
times are in event order and not sorted within a period; sort per burst. The
theory and the reasons are in `doc/alex-alternation-fold.rst`; the checks are
`test/cpp/test_alternation_fold.cpp` and
`test/python/tttr/test_alternation_fold.py`.

## Dependencies

- Depends on `util`, `io/base`, `plugin` and the format modules
  (`io_pq`, `io_bh`, `io_cz`, `io_sm`, `io_ps`, `io_be`, `io_fl`, `io_hdf5`),
  nlohmann/json.

## Examples

- `examples/tttr/plot_write_read_roundtrip_simulated.py` (+ `.ipynb`): a simulated photon stream written as PTU / HT3 / SPC-130 with `TTTR.write` and read back record-for-record — no instrument file needed. The decoders are photon-for-photon identical to phconvert and ptufile (`benchmarks/check_reading.py`).
