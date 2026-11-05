# `spectroscopy/burstpipeline` — the fused burst pipeline

One native pass from a TTTR photon stream to a burst table, per-burst micro-time
decays and per-burst decay fits, with every result held in buffers the result
object owns and exposed to Python as zero-copy NumPy views.

The design, and what it replaces in ChiSurf, is the OKF concept
`chisurf/okf/architecture/fused-burst-pipeline.md`. Measured numbers are in
`PERF.md`, section "Fused burst pipeline".

## Contents

- **`include/BurstPipeline.h`, `src/BurstPipeline.cpp`**:
  - `BurstPipeline` takes a JSON configuration. It has three methods:
    - `run(tttr)` searches, selects, summarises and histograms the bursts. It also fits them when the configuration has `fits`.
    - `decays(tttr, first, last, binning, n_bins)` computes the decays of bursts you already have.
    - `fit(result, config)` fits the CSR decays of a result in place.
  - `BurstPipelineResult` holds the columns and the CSR decays.
  - `StrictBackground` is the background-photon selection of ChiSurf's `strict_background_mask`, kept as an object so the caller can tighten it.
- Bindings: `ext/python/BurstPipeline.i`, `ext/python/burstpipeline_support.py` (the owner-keeping view) and `ext/python/BurstPipelineResult.py` (`column_view`, `columns`, `decays`, `decay_sum`, `dense_decays`, `outside_histogram`, `stats`).

It is a module of its own because it is where the burst search (`burst`) and the decay fits (`decay`) meet. `burst` does not depend on `decay`.

## Configuration (`BurstPipeline(config)`)

```json
{
  "detectors": [{"name": "G|G", "chs": [3, 0], "gates": [[0, 250]]}, ...],
  "windows":   [{"name": "prompt", "lo": 10, "hi": 250}, ...],
  "search": {"algorithm": "cusum_sprt" | "sliding_window",
             "scope": "detectors" | "all",
             "detectors": [...],                      // optional; default: "detectors"
             "parameters": {"min_photons": 5, "background_cps": 3317.16,
                            "signal_to_background_ratio": 3.3, "alpha": 0.05, "beta": 0.05,
                            "L": 20, "m": 10, "T": 0.0005}},
  "selection": {"filter_active": true, "invert": false, "max_gap": 3, "min_photons": 30,
                "dt_min_active": false, "dt_min_ms": 0.001,
                "dt_max_active": true, "dt_max_ms": 0.15,
                "channels": [], "micro_time_ranges": []},
  "decays": {"binning": 1, "n_bins": 0},
  "features": true, "confidence": true,
  "micro_time_resolution_ns": -1, "macro_time_resolution": 0, "feature_macro_time_resolution": 0,
  "chunk_size": 1048576, "n_threads": 0,
  "fits": []
}
```

How the fields are read:

- **Gates and windows** are half-open `[lo, hi)` intervals in raw micro-time channels.
- **Polarisation.** A detector's `chs[::2]` channels are parallel and its `chs[1::2]` channels are perpendicular. A detector with one channel has no polarisation: all its photons are parallel, and the fit uses them for both halves.
- **Search scope.**
  - `"detectors"`: the search runs on the union of the detectors (channel ∩ gate), like ChiSurf's `_search_detectors`.
  - `"all"`: the search runs on the whole stream.
- **Search algorithms.** Only `cusum_sprt` and `sliding_window` are fused. Any other name is refused, so the caller can fall back to the unfused path.
- **Selection.** A photon is selected when all of these hold:
  - it is in a search burst;
  - it passes the delta-macro-time filter (the gap to the previous photon of any channel, in ms; the first photon's gap is 0);
  - it passes the `channels` / `micro_time_ranges` pre-filter.

  After that:
  - `invert` flips the selection.
  - Runs separated by at most `max_gap + 1` unselected photons are merged.
  - Runs shorter than `max(2, min_photons)` are dropped.
- **Defaults.** `n_bins = 0` means the header's micro-time channels divided by `binning`. Resolutions of 0 or below mean "take them from the header".
- **Threads.** `n_threads = 0` uses `TTTRLIB_NUM_THREADS`, then `OMP_NUM_THREADS`, then the number of cores (at most 8).

## Output columns (`BurstPipelineResult`)

One row per burst, in photon order.

**Whole-burst columns:**

| Column | Type | Content |
|---|---|---|
| `first_photon`, `last_photon` | int64 | Inclusive photon indices. |
| `n_photons` | int64 | Photons in the burst. |
| `duration_ms` | float64 | Duration in ms. |
| `mean_macro_time_ms` | float64 | Midpoint of the first and last photon, in ms. |
| `count_rate_khz` | float64 | Photons per ms; NaN for a zero duration. |
| `confidence_sigma` | float64 | `TTTR::burst_confidence` of the final bursts. |

**Stream columns.** Each of these exists once per stream, with the stream name after a `|`:

| Column | Type | Content |
|---|---|---|
| `first_photon\|S`, `last_photon\|S` | int64 | First and last photon of the stream inside the burst. |
| `n_photons\|S` | int64 | Photons of the stream inside the burst. |
| `duration_ms\|S` | float64 | Duration between the stream's first and last photon. |
| `mean_macro_time_ms\|S` | float64 | Midpoint of the stream's first and last photon. |
| `count_rate_khz\|S` | float64 | Photons per ms; NaN for a zero duration. |
| `mean_micro_time_ns\|S` | float64 | Mean of the raw micro times × `micro_time_resolution_ns`. |

- The streams are each detector (`S` = `G|G`), then each (window, detector) pair (`S` = `prompt@G|G`).
- When a stream has no photon in a burst, its columns hold -1 (0 for `n_photons`). This is ChiSurf's `_SPAN_EMPTY`.

**Decays:**

- One CSR matrix per decay group. A decay group is a distinct detector channel list, so R|G and R|R, which share channels 2 and 1, share one matrix.
- `decays(g)` returns `(indptr, bins, counts)`.
- The bin index is `role * n_bins + micro // binning`. Only bins below `n_bins` are kept.
- `decay_sum(g)` is the decays of all bursts added together.

**Histograms:** `outside_histogram()` is the raw micro-time histogram, per detector channel, of the photons that lie outside every burst.

**Fit columns.** `fit(result, {"fits": [...]})` writes these, one set per fitted detector `D`:

| Column | Type | Content |
|---|---|---|
| `fit\|D\|ng_p`, `fit\|D\|ng_s` | int64 | Photons in the VV and VH windows. |
| `fit\|D\|reason` | int64 | Rejection code of ChiSurf's `reject_reason` (see below). |
| `fit\|D\|objective` | float64 | 2I*. |
| `fit\|D\|x\|i` | float64 | Fitted parameter `i`. |
| `fit\|D\|r\|j` | float64 | Entry `j` of the model's result vector. |

Rejection codes (`fit|D|reason`):

| Code | Meaning |
|---|---|
| 0 | Accepted. |
| 1 | Too few photons. |
| 2 | Not converged. |
| 3 | Not finite. |
| 4 | Tau at or below half a bin. |
| 5 | Tau longer than the fit window. |
| 6 | Gamma at 0.99 or above. |

Fit entry:

```json
{"detector": "G|G", "model": "fit23" | "fit24" | "fit25" | "fit26" | "tail",
 "setup": [...], "irf": [...], "background": [...], "x0": [...], "link": [...],
 "n": 500, "fit_start": 0, "fit_stop": -1, "sb": 0, "eb": 250, "vh_sb": 0, "vh_eb": 250,
 "shift": 0, "min_photons": 20, "dt": 0.064, "converged_index": 1,
 "reject_lo": 0, "reject_hi": 250, "tail_start": -1}
```

- `irf` and `background` are the fitted halves (`2 * fit_stop` long) in the form `Fit2x` hands to the model.
- `x0` is the full parameter vector, and `link` holds the constraint codes (-1 = fixed).
- `set_burst_backgrounds(k, matrix)` gives fit entry `k` one background per burst.
- The tail estimate needs no IRF.

## Progress and cancellation

These are read and set from another thread; the calls release the GIL.

- `progress_photons()` and `progress_total()` report how far the pass has read.
- `progress_bursts()` and `progress_fits()` count finished bursts and fitted bursts.
- `cancel()` stops the call at the next chunk. The result is then marked `cancelled()`.
- `calls()` counts the calls into the object, which is how the crossings are counted.

## Dependencies

`util`, `math`, `core`, `plugin`, `burst` (`TTTR::burst_confidence`), `decay` (`make_decay_fit`), nlohmann/json.

## Tests

- `test/cpp/test_burst_pipeline.cpp` checks the pipeline against the unfused path, which it builds from the library's own pieces (a subset TTTR, the search, the masks and `BurstFilter::bursts_from_mask`). It runs at a chunk size of 1024 and of 1 M.
- `test/python/burstpipeline/` checks the bindings, the zero-copy views and the fits against `DecayFit2.fit_many`.
- The equivalence with ChiSurf on DNA13bp_3 is in ChiSurf: `test/core/test_fused_burst_pipeline.py`, and `build_tools/dev_utils/fused_pipeline_equivalence.py` on home.
