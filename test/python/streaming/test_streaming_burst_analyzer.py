"""StreamingBurstAnalyzer against the batch burst table, for every preset.

The analyzer is the live twin of the batch path a recorded measurement goes
through: select the detectors' channels, run `TTTR::burst_search_sliding_window`
on the selection, count each burst per detector and per window (the table
`tttr sm` and ChiSurf's `generate_burst_dataframe` write), and derive quantities
from those counts. So, for single colour, two colour, PIE, µs-ALEX, MFD and
three-colour PIE:

* the same photons pushed in ANY chunking give the batch table column for
  column (names, order, sentinels, values);
* every derived quantity equals its batch formula on the batch counts:
  `tttrlib.apparent_es`, `tttrlib.corrected_es` (background = rate x duration),
  DecayFit's steady-state anisotropy, plain count ratios;
* on a simulated PIE measurement the known populations come back.

The batch reference is written out here in numpy over a `TTTR` built from the
selected photons, so this file needs neither the CLI binary nor test data.
"""
import numpy as np
import pytest

import tttrlib

DT = 12.5e-9          # macro time resolution [s]
MICRO_DT = 16e-12     # micro time resolution [s]
L, M, T = 20, 10, 1e-4
ALEX_PERIOD = 100e-6  # s

SETUPS = {
    "single_color": {"detectors": {"green": {"chs": [0, 2]}}},
    "two_color": {"detectors": {"green": {"chs": [0, 2]}, "red": {"chs": [1, 3]}}},
    "pie": {"detectors": {"green": {"chs": [0, 2]}, "red": {"chs": [1, 3]}},
            "windows": {"prompt": [0, 2048], "delayed": [2048, 4096]}},
    "alex": {"detectors": {"green": {"chs": [0, 2]}, "red": {"chs": [1, 3]}}},
    "mfd": {"detectors": {"green_par": {"chs": [0], "g_factor": 1.07, "l1": 0.03, "l2": 0.02},
                          "green_perp": {"chs": [2]},
                          "red_par": {"chs": [1], "g_factor": 0.95, "l1": 0.01, "l2": 0.04},
                          "red_perp": {"chs": [3]}},
            "windows": {"prompt": [0, 2048], "delayed": [2048, 4096]}},
    "three_color_pie": {"detectors": {"blue": {"chs": [4]}, "green": {"chs": [0, 2]},
                                      "red": {"chs": [1, 3]}},
                        "windows": {"blue": [0, 1365], "green": [1365, 2730], "red": [2730, 4096]}},
}


# ---------------------------------------------------------------------------
# a synthetic stream: background (incl. an undeclared channel 7) and bursts


def synthetic(seed, n_bursts=250):
    rng = np.random.default_rng(seed)
    times, micro, ch = [], [], []
    t = 0.0
    for b in range(n_bursts):
        n_bg = rng.poisson(40)
        tb = t + np.sort(rng.uniform(0, 0.01 / DT, n_bg))
        times.append(tb); micro.append(rng.integers(0, 4096, n_bg))
        ch.append(rng.choice([0, 1, 2, 3, 4, 7], n_bg))
        t = tb[-1] if n_bg else t + 0.01 / DT
        n = rng.integers(40, 140)
        tt = t + np.cumsum(1.0 + np.floor(rng.exponential(5e-6 / DT, n)))
        e = 0.25 if b % 2 else 0.75
        exc = rng.integers(0, 3, n)              # 0 blue, 1 green(donor), 2 red(acceptor) laser
        mi = np.where(exc == 0, rng.integers(0, 1365, n),
                      np.where(exc == 1, rng.integers(1365, 2730, n), rng.integers(2730, 4096, n)))
        mi = np.where(rng.uniform(size=n) < 0.5, mi % 2048, mi)       # PIE halves too
        acc = np.where(exc == 2, rng.uniform(size=n) < 0.95, rng.uniform(size=n) < e)
        colour = np.where(exc == 0, np.where(rng.uniform(size=n) < 0.3, 4, np.where(acc, 1, 0)),
                          np.where(acc, 1, 0))
        pol = np.where((colour < 4) & (rng.uniform(size=n) < 0.4), 2, 0)
        times.append(tt); micro.append(mi); ch.append(colour + pol)
        t = tt[-1]
    macro = np.floor(np.concatenate(times)).astype(np.uint64)
    order = np.argsort(macro, kind="stable")
    return (macro[order], np.concatenate(micro)[order].astype(np.uint16),
            np.concatenate(ch)[order].astype(np.int32))


# ---------------------------------------------------------------------------
# the batch reference


def batch_table(macro, micro, ch, setup, windows, L=L, m=M, T=T):
    """`windows`: [(name, kind, lo, hi, period)] with kind 'micro' or 'phase'."""
    dets = setup["detectors"]
    used = sorted({c for d in dets.values() for c in d["chs"]})
    sel = np.isin(ch, used)
    mt, mi, rc = macro[sel], micro[sel], ch[sel]
    tt = tttrlib.TTTR()
    tt.append_events(mt, mi, rc.astype(np.int8), np.zeros(len(mt), np.int8))
    tt.header.set_macro_time_resolution(DT)
    pairs = np.asarray(tt.burst_search_sliding_window(L, m, T), dtype=np.int64).reshape(-1, 2)
    ms, nan = DT * 1e3, np.nan

    def det_mask(d):
        ok = np.isin(rc, d["chs"])
        rr = d.get("micro_time_ranges") or []
        if rr:
            ok &= np.any([(mi >= lo) & (mi < hi) for lo, hi in rr], axis=0)
        return ok

    dm = {n: det_mask(d) for n, d in dets.items()}
    wm = {}
    for name, kind, lo, hi, period in windows:
        v = (mt % np.uint64(period)) if kind == "phase" else mi
        wm[name] = (v >= lo) & (v < hi)

    cols = {k: [] for k in ("First Photon", "Last Photon", "Duration (ms)",
                            "Mean Macro Time (ms)", "Number of Photons", "Count Rate (KHz)")}
    for s, e in pairs:
        t0, t1 = float(mt[s]), float(mt[e])
        dur, n = (t1 - t0) * ms, e - s + 1
        for k, v in zip(cols, (s, e, dur, (t1 + t0) / 2 * ms, n, n / dur if dur > 0 else nan)):
            cols[k].append(v)
    counts = {}

    def span_cols(mask):
        f, l, du, mm, nn, rate = [], [], [], [], [], []
        for s, e in pairs:
            idx = s + np.flatnonzero(mask[s:e + 1])
            if len(idx) == 0:
                f.append(-1); l.append(-1); du.append(-1.0); mm.append(-1.0); nn.append(0); rate.append(-1.0)
                continue
            a, b = float(mt[idx[0]]), float(mt[idx[-1]])
            d_ms = (b - a) * ms
            f.append(idx[0]); l.append(idx[-1]); du.append(d_ms); mm.append((b + a) / 2 * ms)
            nn.append(len(idx)); rate.append(len(idx) / d_ms if d_ms > 0 else nan)
        return f, l, du, mm, nn, rate

    for name, mask in dm.items():
        f, l, du, mm, nn, rate = span_cols(mask)
        cols[f"First Photon ({name})"] = f; cols[f"Last Photon ({name})"] = l
        cols[f"Duration ({name}) (ms)"] = du; cols[f"Mean Macrotime ({name}) (ms)"] = mm
        cols[f"Number of Photons ({name})"] = nn; cols[f"{name.capitalize()} Count Rate (KHz)"] = rate
        counts[name] = np.asarray(nn, float)
    for wname, kind, lo, hi, period in windows:
        for dname, mask in dm.items():
            f, l, du, mm, nn, rate = span_cols(mask & wm[wname])
            stream, rng_ = f"S {wname} {dname}", f" | {lo}-{hi}"
            cols[stream + " (kHz)" + rng_] = rate
            cols[stream + " (photons)" + rng_] = nn
            cols[stream + " (ms)" + rng_] = du
            counts[f"{wname} {dname}"] = np.asarray(nn, float)
    for dname, mask in dm.items():
        v = []
        for s, e in pairs:
            idx = s + np.flatnonzero(mask[s:e + 1])
            v.append(float(np.mean(mi[idx].astype(float))) * MICRO_DT * 1e9 if len(idx) else -1.0)
        cols[f"Mean Microtime ({dname}) (ns)"] = v
    table = {k: np.asarray(v, dtype=float) for k, v in cols.items()}
    return table, counts, pairs


def windows_of(preset):
    if preset == "alex":
        period = int(round(ALEX_PERIOD / DT))
        return [("prompt", "phase", 0, period // 2, period), ("delayed", "phase", period // 2, period, period)]
    return [(n, "micro", lo, hi, 0) for n, (lo, hi) in (SETUPS[preset].get("windows") or {}).items()]


def make(preset, **kw):
    kwargs = dict(min_photons=L, window_photons=M, window_time=T,
                  macro_time_resolution=DT, micro_time_resolution=MICRO_DT)
    if preset == "alex":
        kwargs["alex_period"] = ALEX_PERIOD
    kwargs.update(kw)
    return tttrlib.StreamingBurstAnalyzer.from_setup(SETUPS[preset], preset=preset, **kwargs)


def push_split(a, macro, micro, ch, splits):
    for idx in np.array_split(np.arange(len(macro)), splits):
        a.push_np(macro[idx], micro[idx], ch[idx])
    a.flush()


# ---------------------------------------------------------------------------
# batch agreement, every preset


@pytest.mark.parametrize("preset", list(SETUPS))
@pytest.mark.parametrize("splits", [1, 7, 997])
def test_table_equals_the_batch_in_any_chunking(preset, splits):
    macro, micro, ch = synthetic(1)
    ref, _, pairs = batch_table(macro, micro, ch, SETUPS[preset], windows_of(preset))
    a = make(preset)
    push_split(a, macro, micro, ch, splits)
    got = a.table()
    assert len(pairs) > 100
    base = list(ref)
    assert list(a.table_names())[:len(base)] == base, "column names or order differ from the batch"
    for name in base:
        np.testing.assert_allclose(got[name].astype(float), ref[name], rtol=1e-12, atol=0,
                                   equal_nan=True, err_msg=f"{preset}: {name}")


@pytest.mark.parametrize("preset", list(SETUPS))
def test_uneven_chunks_including_empty_ones(preset):
    macro, micro, ch = synthetic(5)
    cuts = np.sort(np.random.default_rng(0).integers(0, len(macro), 60))
    a, one = make(preset), make(preset)
    prev = 0
    for c in list(cuts) + [len(macro)]:
        a.push_np(macro[prev:c], micro[prev:c], ch[prev:c])
        prev = c
    a.flush()
    one.push_np(macro, micro, ch)
    one.flush()
    for name in one.table_names():
        np.testing.assert_array_equal(a.table()[name], one.table()[name], err_msg=name)
    for name, h in one.histograms().items():
        np.testing.assert_array_equal(a.histograms()[name]["counts"], h["counts"], err_msg=name)


# ---------------------------------------------------------------------------
# derived quantities equal their batch formulas on the batch counts


def test_two_color_proximity_ratio():
    macro, micro, ch = synthetic(2)
    _, c, _ = batch_table(macro, micro, ch, SETUPS["two_color"], [])
    a = make("two_color")
    push_split(a, macro, micro, ch, 3)
    np.testing.assert_allclose(a.table()["Proximity Ratio"],
                               tttrlib.apparent_es(c["green"], c["red"])["E"], rtol=0, atol=1e-12)


@pytest.mark.parametrize("preset", ["pie", "alex"])
def test_pie_and_alex_raw_and_corrected_es(preset):
    macro, micro, ch = synthetic(3)
    _, c, _ = batch_table(macro, micro, ch, SETUPS[preset], windows_of(preset))
    f = {"gamma": 1.3, "beta": 0.85, "alpha": 0.07, "delta": 0.05}
    a = make(preset, factors=f, subtract_background=False)
    push_split(a, macro, micro, ch, 7)
    t = a.table()
    dd, da, aa = c["prompt green"], c["prompt red"], c["delayed red"]
    raw = tttrlib.apparent_es(dd, da, aa)
    np.testing.assert_allclose(t["Proximity Ratio"], raw["E"], rtol=0, atol=1e-12)
    np.testing.assert_allclose(t["Stoichiometry"], raw["S"], rtol=0, atol=1e-12)
    cor = tttrlib.corrected_es(dd, da, aa, **f)
    np.testing.assert_allclose(t["FRET Efficiency"], cor["E"], rtol=0, atol=1e-12)
    np.testing.assert_allclose(t["Stoichiometry (corrected)"], cor["S"], rtol=0, atol=1e-12)


def test_corrected_es_with_fixed_backgrounds():
    macro, micro, ch = synthetic(4)
    _, c, _ = batch_table(macro, micro, ch, SETUPS["pie"], windows_of("pie"))
    f = {"gamma": 1.1, "beta": 0.9, "alpha": 0.04, "delta": 0.06}
    a = make("pie", factors=f, subtract_background=True)
    for s, r in (("prompt green", 1200.0), ("prompt red", 700.0), ("delayed red", 900.0)):
        a.set_background_rate(s, r)
    push_split(a, macro, micro, ch, 5)
    t = a.table()
    span = t["Duration (ms)"] * 1e-3
    cor = tttrlib.corrected_es(c["prompt green"], c["prompt red"], c["delayed red"], **f,
                               bg_dd=1200.0 * span, bg_da=700.0 * span, bg_aa=900.0 * span)
    np.testing.assert_allclose(t["FRET Efficiency"], cor["E"], rtol=0, atol=1e-9)
    np.testing.assert_allclose(t["Stoichiometry (corrected)"], cor["S"], rtol=0, atol=1e-9)


def test_mfd_anisotropy_equals_the_batch_values():
    macro, micro, ch = synthetic(6)
    setup = SETUPS["mfd"]
    _, c, _ = batch_table(macro, micro, ch, setup, windows_of("mfd"))
    a = make("mfd")
    push_split(a, macro, micro, ch, 11)
    t = a.table()
    for colour in ("green", "red"):
        d = setup["detectors"][colour + "_par"]
        g, l1, l2 = d["g_factor"], d["l1"], d["l2"]
        fp, fs = c[f"prompt {colour}_par"], c[f"prompt {colour}_perp"]
        den = fp * (1 - 3 * l2) + (2 - 3 * l1) * g * fs
        ref = np.where(den != 0, (fp - g * fs) / np.where(den != 0, den, 1), 0.0)
        np.testing.assert_allclose(t[f"r Experimental ({colour})"], ref, rtol=0, atol=1e-12,
                                   err_msg=colour)
    don = c["prompt green_par"] + c["prompt green_perp"]
    acc = c["prompt red_par"] + c["prompt red_perp"]
    np.testing.assert_allclose(t["Proximity Ratio"], tttrlib.apparent_es(don, acc)["E"],
                               rtol=0, atol=1e-12)


def test_three_color_ratios():
    macro, micro, ch = synthetic(7)
    _, c, _ = batch_table(macro, micro, ch, SETUPS["three_color_pie"], windows_of("three_color_pie"))
    a = make("three_color_pie")
    push_split(a, macro, micro, ch, 4)
    t = a.table()

    def ratio(num, den):
        n = sum(c[k] for k in num); d = sum(c[k] for k in den)
        return np.where(d != 0, n / np.where(d != 0, d, 1), 0.0)

    np.testing.assert_allclose(t["Proximity Ratio (BG)"], ratio(["blue green"], ["blue blue", "blue green"]), atol=1e-12)
    np.testing.assert_allclose(t["Proximity Ratio (BR)"], ratio(["blue red"], ["blue blue", "blue red"]), atol=1e-12)
    np.testing.assert_allclose(t["Proximity Ratio (GR)"], ratio(["green red"], ["green green", "green red"]), atol=1e-12)
    np.testing.assert_allclose(t["Stoichiometry (GR)"],
                               ratio(["green green", "green red"], ["green green", "green red", "red red"]), atol=1e-12)


# ---------------------------------------------------------------------------
# accumulators, snapshot, setup rules


def test_histograms_snapshot_trace_and_decays():
    macro, micro, ch = synthetic(8)
    a = make("pie", trace=(1e-3, 50), decays=(64, 4096, True))
    rev0 = a.revision()
    push_split(a, macro, micro, ch, 5)
    snap = a.snapshot()
    assert snap["revision"] > rev0 and snap["bursts"] == a.burst_count() > 100
    h = snap["histograms"]["Proximity Ratio vs Stoichiometry"]
    e, s = a.table()["Proximity Ratio"], a.table()["Stoichiometry"]
    inside = (e >= -0.1) & (e < 1.1) & (s >= -0.1) & (s < 1.1)
    ref, _, _ = np.histogram2d(e[inside], s[inside], bins=[h["x_edges"], h["y_edges"]])
    np.testing.assert_array_equal(h["counts"], ref)
    assert set(snap["trace"]["counts"]) == set(a.stream_names())
    assert len(snap["trace"]["counts"]["green"]) == 50
    for st in a.stream_names():
        assert snap["decays"][st].sum() == a.stream_photons(st)
    assert snap["burst_decays"]["red"].sum() == a.table()["Number of Photons (red)"].sum()
    # a histogram added later is filled from the table
    i = a.add_histogram("Duration (ms)", 20, 0.0, 5.0, "", 0, 0.0, 0.0)
    d = a.table()["Duration (ms)"]
    assert np.asarray(a.histogram(i)).sum() == ((d >= 0) & (d < 5.0)).sum()


def test_live_background_per_stream():
    macro, micro, ch = synthetic(9)
    a = make("pie")
    push_split(a, macro, micro, ch, 9)
    bg = a.snapshot()["background_cps"]
    assert all(bg[s] > 0 for s in ("prompt green", "prompt red", "delayed red"))
    assert all(bg[s] < 3000 for s in bg)


def test_setup_rules():
    a = make("pie")
    a.push_np(np.array([1, 2, 3], np.uint64), np.zeros(3, np.uint16), np.zeros(3, np.int32))
    with pytest.raises(Exception):
        a.add_window("late", 0, 10)
    a.clear()
    a.add_window("late", 0, 10)
    with pytest.raises(Exception, match="not implemented yet"):
        tttrlib.StreamingBurstAnalyzer(DT, MICRO_DT).set_search_mode("dcbs")
    with pytest.raises(ValueError):
        tttrlib.StreamingBurstAnalyzer.from_setup(SETUPS["pie"], preset="nonsense")
    with pytest.raises(Exception):
        tttrlib.StreamingBurstAnalyzer(DT, MICRO_DT).add_ratio("x", ["nope"], ["nope"], False)


# ---------------------------------------------------------------------------
# a simulated PIE measurement: known populations come back


@pytest.fixture(scope="module")
def simulated_pie():
    if not hasattr(tttrlib, "SimEngine"):
        pytest.skip("tttrlib built without the photon simulator")
    # Doubly-labelled diffusing molecules, two populations E = 0.2 and 0.8 at
    # S ~ 0.5 (green row DD+DA = 300, red row AA = 300). The simulator alternates
    # the lasers in macro time (ALEX); folding each photon's laser into the micro
    # time (laser 0 -> prompt half, laser 1 -> delayed half) is exactly a PIE
    # stream, with the laser known per photon.
    cfg = {
        "settings": {"dt": 0.01, "n_ph_max": 300000, "n_channels": 2,
                     "n_microtime_channels": 4096, "microtime_resolution": 0.008,
                     "laser_period": 32.0, "alex_period": 0.1, "alex_markers": False,
                     "active_margin": 1.0, "fast_grid_bbox": True,
                     "seed_diffusion": 4242, "seed_emission": 2424},
        "box": {"xy": 2.0, "z": 4.0},
        "species": [
            {"D": 0.05, "q_alex": [[240.0, 60.0], [0.0, 300.0]],
             "decay": {"lifetimes": [3.8], "amplitudes": [1.0], "dt": 0.008}},
            {"D": 0.05, "q_alex": [[60.0, 240.0], [0.0, 300.0]],
             "decay": {"lifetimes": [1.6], "amplitudes": [1.0], "dt": 0.008}},
        ],
        "k_rad": [0, 0, 0, 0], "k_nrad": [0, 0, 0, 0],
        "background": [0.02, 0.02], "population": [0.15, 0.15],
        "excitation": [
            {"type": "gaussian3d", "w0": 0.3, "z0": 2.0, "extent_xy": 2.0, "extent_z": 4.0, "spacing": 0.1},
            {"type": "gaussian3d", "w0": 0.3, "z0": 2.0, "extent_xy": 2.0, "extent_z": 4.0, "spacing": 0.1},
        ],
    }
    e = tttrlib.SimEngine.from_dict(cfg)
    e.run()
    ph = e.photons()
    photon = ph["event_type"] == 0
    wpl = int(round(cfg["settings"]["alex_period"] / cfg["settings"]["dt"] / 2))
    laser = ((ph["macro_window"] // wpl) % 2)[photon]
    macro = np.floor((ph["macro_window"][photon].astype(float) * cfg["settings"]["dt"]
                      + ph["arrival_time"][photon].astype(float)) / (DT * 1e3)).astype(np.uint64)
    micro = (ph["micro_time"][photon].astype(np.int64) // 2 + 2048 * laser).astype(np.uint16)
    ch = ph["channel"][photon].astype(np.int32)
    order = np.argsort(macro, kind="stable")
    return macro[order], micro[order], ch[order]


@pytest.mark.heavy
def test_simulated_pie_populations_are_recovered(simulated_pie):
    macro, micro, ch = simulated_pie
    setup = {"detectors": {"green": {"chs": [0]}, "red": {"chs": [1]}},
             "windows": {"prompt": [0, 2048], "delayed": [2048, 4096]}}
    a = tttrlib.StreamingBurstAnalyzer.from_setup(
        setup, preset="pie", min_photons=30, window_photons=10, window_time=5e-4,
        macro_time_resolution=DT, micro_time_resolution=MICRO_DT)
    push_split(a, macro, micro, ch, 23)
    t = a.table()
    e, s = t["Proximity Ratio"], t["Stoichiometry"]
    assert len(e) > 60, f"too few bursts in the simulation: {len(e)}"
    double = (s > 0.25) & (s < 0.75)
    lo, hi = e[double & (e < 0.5)], e[double & (e >= 0.5)]
    assert len(lo) > 15 and len(hi) > 15
    assert np.median(lo) == pytest.approx(0.2, abs=0.07)
    assert np.median(hi) == pytest.approx(0.8, abs=0.07)
    assert np.median(s[double]) == pytest.approx(0.5, abs=0.1)
    ref, _, pairs = batch_table(macro, micro, ch, setup,
                                [("prompt", "micro", 0, 2048, 0), ("delayed", "micro", 2048, 4096, 0)],
                                L=30, m=10, T=5e-4)
    np.testing.assert_array_equal(t["First Photon"], pairs[:, 0])
    np.testing.assert_array_equal(t["S delayed red (photons) | 2048-4096"],
                                  ref["S delayed red (photons) | 2048-4096"])
