"""StreamingPieBurstAnalyzer against the batch burst table.

The analyzer is the live twin of the batch path a recorded measurement goes
through: select the detectors' channels, run `TTTR::burst_search_sliding_window`
on the selection, count each burst per detector and per micro-time window (the
table `tttr sm` and ChiSurf's `generate_burst_dataframe` write), and turn the
counts into E and S with `apparent_es` / `corrected_es`. So:

* the same photons, pushed in ANY chunking, give the batch table column for
  column (names, order, sentinels, values);
* raw E/S equal `tttrlib.apparent_es` and corrected E/S equal
  `tttrlib.corrected_es` with the per-burst background (rate x duration);
* on a simulated PIE smFRET measurement the known populations come back.

The batch reference is written out here in numpy over a `TTTR` built from the
selected photons, so this file needs neither the CLI binary nor test data.
"""
import numpy as np
import pytest

import tttrlib

DT = 12.5e-9          # macro time resolution [s]
MICRO_DT = 16e-12     # micro time resolution [s]
SETUP = {
    "detectors": {"green": {"chs": [0, 2]}, "red": {"chs": [1, 3]}},
    "windows": {"prompt": [0, 2048], "delayed": [2048, 4096]},
}
L, M, T = 20, 10, 1e-4


# ---------------------------------------------------------------------------
# streams


def synthetic_pie(seed, n_bursts=300):
    """Background (incl. an undeclared channel 7) with planted PIE bursts of two
    FRET populations; integer macro times, coincident ticks possible."""
    rng = np.random.default_rng(seed)
    times, micro, ch = [], [], []
    t = 0.0
    for b in range(n_bursts):
        n_bg = rng.poisson(40)
        tb = t + np.sort(rng.uniform(0, 0.01 / DT, n_bg))
        times.append(tb)
        micro.append(rng.integers(0, 4096, n_bg))
        ch.append(rng.choice([0, 1, 2, 3, 7], n_bg))
        t = tb[-1] if n_bg else t + 0.01 / DT
        n = rng.integers(40, 140)
        tt = t + np.cumsum(1.0 + np.floor(rng.exponential(5e-6 / DT, n)))
        e = 0.25 if b % 2 else 0.75
        donor_exc = rng.uniform(size=n) < 0.5
        mi = np.where(donor_exc, rng.integers(0, 2048, n), rng.integers(2048, 4096, n))
        acc = np.where(donor_exc, rng.uniform(size=n) < e, rng.uniform(size=n) < 0.95)
        c = np.where(acc, rng.choice([1, 3], n), rng.choice([0, 2], n))
        times.append(tt); micro.append(mi); ch.append(c)
        t = tt[-1]
    macro = np.floor(np.concatenate(times)).astype(np.uint64)
    order = np.argsort(macro, kind="stable")
    return (macro[order], np.concatenate(micro)[order].astype(np.uint16),
            np.concatenate(ch)[order].astype(np.int32))


# ---------------------------------------------------------------------------
# the batch reference


def batch_table(macro, micro, ch, setup=SETUP, dt=DT, micro_dt=MICRO_DT, L=L, m=M, T=T):
    used = sorted({c for d in setup["detectors"].values() for c in d["chs"]})
    sel = np.isin(ch, used)
    mt, mi, rc = macro[sel], micro[sel], ch[sel]
    tt = tttrlib.TTTR()
    tt.append_events(mt, mi, rc.astype(np.int8), np.zeros(len(mt), np.int8))
    tt.header.set_macro_time_resolution(dt)
    pairs = np.asarray(tt.burst_search_sliding_window(L, m, T), dtype=np.int64).reshape(-1, 2)
    ms = dt * 1e3
    nan = np.nan

    def det_mask(d):
        ok = np.isin(rc, d["chs"])
        rr = d.get("micro_time_ranges") or []
        if rr:
            ok &= np.any([(mi >= lo) & (mi < hi) for lo, hi in rr], axis=0)
        return ok

    dets = {n: det_mask(d) for n, d in setup["detectors"].items()}
    wins = {n: (mi >= lo) & (mi < hi) for n, (lo, hi) in setup["windows"].items()}
    cols = {k: [] for k in ("First Photon", "Last Photon", "Duration (ms)",
                            "Mean Macro Time (ms)", "Number of Photons", "Count Rate (KHz)")}
    for s, e in pairs:
        t0, t1 = float(mt[s]), float(mt[e])
        dur = (t1 - t0) * ms
        n = e - s + 1
        cols["First Photon"].append(s); cols["Last Photon"].append(e)
        cols["Duration (ms)"].append(dur); cols["Mean Macro Time (ms)"].append((t1 + t0) / 2 * ms)
        cols["Number of Photons"].append(n); cols["Count Rate (KHz)"].append(n / dur if dur > 0 else nan)
    for name, dm in dets.items():
        f, l, du, mm, nn, rate = [], [], [], [], [], []
        for s, e in pairs:
            idx = s + np.flatnonzero(dm[s:e + 1])
            if len(idx) == 0:
                f.append(-1); l.append(-1); du.append(-1.0); mm.append(-1.0); nn.append(0); rate.append(-1.0)
                continue
            a, b = float(mt[idx[0]]), float(mt[idx[-1]])
            d_ms = (b - a) * ms
            f.append(idx[0]); l.append(idx[-1]); du.append(d_ms); mm.append((b + a) / 2 * ms)
            nn.append(len(idx)); rate.append(len(idx) / d_ms if d_ms > 0 else nan)
        cols[f"First Photon ({name})"] = f; cols[f"Last Photon ({name})"] = l
        cols[f"Duration ({name}) (ms)"] = du; cols[f"Mean Macrotime ({name}) (ms)"] = mm
        cols[f"Number of Photons ({name})"] = nn; cols[f"{name.capitalize()} Count Rate (KHz)"] = rate
    for wn, wm in wins.items():
        lo, hi = setup["windows"][wn]
        for dn, dm in dets.items():
            stream = f"S {wn} {dn}"
            rng_ = f" | {lo}-{hi}"
            r_, n_, d_ = [], [], []
            for s, e in pairs:
                idx = s + np.flatnonzero((dm & wm)[s:e + 1])
                if len(idx) == 0:
                    r_.append(-1.0); n_.append(0); d_.append(-1.0)
                    continue
                d_ms = (float(mt[idx[-1]]) - float(mt[idx[0]])) * ms
                r_.append(len(idx) / d_ms if d_ms > 0 else nan); n_.append(len(idx)); d_.append(d_ms)
            cols[stream + " (kHz)" + rng_] = r_
            cols[stream + " (photons)" + rng_] = n_
            cols[stream + " (ms)" + rng_] = d_
    for dn, dm in dets.items():
        v = []
        for s, e in pairs:
            idx = s + np.flatnonzero(dm[s:e + 1])
            v.append(float(np.mean(mi[idx].astype(float))) * micro_dt * 1e9 if len(idx) else -1.0)
        cols[f"Mean Microtime ({dn}) (ns)"] = v
    return {k: np.asarray(v, dtype=float) for k, v in cols.items()}, pairs, mt


def analyzer(**kw):
    a = tttrlib.StreamingPieBurstAnalyzer.from_setup(
        SETUP, donor="green", acceptor="red", prompt="prompt", delay="delayed",
        min_photons=L, window_photons=M, window_time=T,
        macro_time_resolution=DT, micro_time_resolution=MICRO_DT)
    return a


def push_split(a, macro, micro, ch, splits):
    for idx in np.array_split(np.arange(len(macro)), splits):
        a.push_np(macro[idx], micro[idx], ch[idx])
    a.flush()


# ---------------------------------------------------------------------------


@pytest.mark.parametrize("seed", [1, 2])
@pytest.mark.parametrize("splits", [1, 3, 17, 997])
def test_table_equals_the_batch_in_any_chunking(seed, splits):
    macro, micro, ch = synthetic_pie(seed)
    ref, pairs, _ = batch_table(macro, micro, ch)
    a = analyzer()
    push_split(a, macro, micro, ch, splits)
    got = a.table()
    assert len(pairs) > 100
    base = [n for n in a.table_names() if n not in
            ("Proximity Ratio", "Stoichiometry", "FRET Efficiency", "Stoichiometry (corrected)")]
    assert base == list(ref), "column names or order differ from the batch table"
    for name in base:
        np.testing.assert_allclose(got[name].astype(float), ref[name], rtol=1e-12, atol=0,
                                   equal_nan=True, err_msg=name)


def test_uneven_chunks_including_empty_ones():
    macro, micro, ch = synthetic_pie(5)
    rng = np.random.default_rng(0)
    cuts = np.sort(rng.integers(0, len(macro), 60))
    a = analyzer()
    prev = 0
    for c in list(cuts) + [len(macro)]:
        a.push_np(macro[prev:c], micro[prev:c], ch[prev:c])   # may be empty
        prev = c
    a.flush()
    one = analyzer()
    one.push_np(macro, micro, ch)
    one.flush()
    for name in one.table_names():
        np.testing.assert_array_equal(a.table()[name], one.table()[name], err_msg=name)


def test_raw_and_corrected_es_equal_the_batch_functions():
    macro, micro, ch = synthetic_pie(3)
    a = analyzer()
    a.set_corrections(1.3, 0.85, 0.07, 0.05)        # gamma, beta, alpha, delta
    a.set_background_rates(1200.0, 700.0, 900.0)    # counts / s: DD, DA, AA
    push_split(a, macro, micro, ch, 7)
    t = a.table()
    i_dd = t["S prompt green (photons) | 0-2048"]
    i_da = t["S prompt red (photons) | 0-2048"]
    i_aa = t["S delayed red (photons) | 2048-4096"]
    raw = tttrlib.apparent_es(i_dd, i_da, i_aa)
    np.testing.assert_allclose(t["Proximity Ratio"], raw["E"], rtol=0, atol=1e-12)
    np.testing.assert_allclose(t["Stoichiometry"], raw["S"], rtol=0, atol=1e-12)
    span = (t["Duration (ms)"] * 1e-3)               # seconds
    cor = tttrlib.corrected_es(i_dd, i_da, i_aa, gamma=1.3, beta=0.85, alpha=0.07, delta=0.05,
                               bg_dd=1200.0 * span, bg_da=700.0 * span, bg_aa=900.0 * span)
    np.testing.assert_allclose(t["FRET Efficiency"], cor["E"], rtol=0, atol=1e-9)
    np.testing.assert_allclose(t["Stoichiometry (corrected)"], cor["S"], rtol=0, atol=1e-9)


def test_histogram_and_snapshot():
    macro, micro, ch = synthetic_pie(4)
    a = analyzer()
    a.set_histogram(40, 0.0, 1.0, 20, 0.0, 1.0, False)
    rev0 = a.revision()
    push_split(a, macro, micro, ch, 5)
    snap = a.snapshot()
    assert snap["revision"] > rev0
    assert snap["histogram"].shape == (40, 20)
    e, s = a.table()["Proximity Ratio"], a.table()["Stoichiometry"]
    ref, _, _ = np.histogram2d(e, s, bins=[snap["e_edges"], snap["s_edges"]])
    # numpy closes the last bin on the right; the analyzer's bins are half-open
    inside = (e < 1.0) & (s < 1.0)
    ref2, _, _ = np.histogram2d(e[inside], s[inside], bins=[snap["e_edges"], snap["s_edges"]])
    np.testing.assert_array_equal(snap["histogram"], ref2)
    assert snap["histogram"].sum() <= ref.sum()
    assert snap["bursts"] == a.burst_count() and snap["burst_rate_hz"] > 0
    # re-binning keeps the bursts
    a.set_histogram(10, 0.0, 1.0, 10, 0.0, 1.0, False)
    assert a.histogram.sum() == pytest.approx(ref2.sum())


def test_live_background_is_estimated_between_bursts():
    macro, micro, ch = synthetic_pie(6)
    a = analyzer()
    push_split(a, macro, micro, ch, 9)
    bg = np.asarray(a.background_rates())
    assert np.all(bg[:3] > 0)
    # background photons: ~40 per 10 ms gap over 5 channels (4 declared), 4 streams of 8
    assert np.all(bg[:3] < 2000)


def test_setup_is_locked_while_photons_flow_and_reopens_on_clear():
    a = analyzer()
    a.push_np(np.array([1, 2, 3], np.uint64), np.zeros(3, np.uint16), np.zeros(3, np.int32))
    with pytest.raises(Exception):
        a.add_window("late", 0, 10)
    a.clear()
    a.add_window("late", 0, 10)        # allowed again
    assert a.burst_count() == 0 and a.photon_count() == 0


def test_without_a_delay_window_there_is_no_stoichiometry():
    setup = {"detectors": SETUP["detectors"], "windows": {"prompt": [0, 4096]}}
    a = tttrlib.StreamingPieBurstAnalyzer.from_setup(
        setup, delay=None, min_photons=L, window_photons=M, window_time=T,
        macro_time_resolution=DT, micro_time_resolution=MICRO_DT)
    macro, micro, ch = synthetic_pie(2)
    push_split(a, macro, micro, ch, 2)
    names = a.table_names()
    assert "Proximity Ratio" in names and "Stoichiometry" not in names


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
    macro_res_ms = DT * 1e3        # simulator time is in ms
    macro = np.floor((ph["macro_window"][photon].astype(float) * cfg["settings"]["dt"]
                      + ph["arrival_time"][photon].astype(float)) / macro_res_ms).astype(np.uint64)
    micro = (ph["micro_time"][photon].astype(np.int64) // 2 + 2048 * laser).astype(np.uint16)
    ch = ph["channel"][photon].astype(np.int32)
    order = np.argsort(macro, kind="stable")
    return macro[order], micro[order], ch[order]


def test_simulated_pie_populations_are_recovered(simulated_pie):
    macro, micro, ch = simulated_pie
    setup = {"detectors": {"green": {"chs": [0]}, "red": {"chs": [1]}},
             "windows": {"prompt": [0, 2048], "delayed": [2048, 4096]}}
    a = tttrlib.StreamingPieBurstAnalyzer.from_setup(
        setup, min_photons=30, window_photons=10, window_time=5e-4,
        macro_time_resolution=DT, micro_time_resolution=MICRO_DT)
    push_split(a, macro, micro, ch, 23)
    t = a.table()
    e, s = t["Proximity Ratio"], t["Stoichiometry"]
    assert len(e) > 60, f"too few bursts in the simulation: {len(e)}"
    double = (s > 0.25) & (s < 0.75)                       # doubly labelled
    lo, hi = e[double & (e < 0.5)], e[double & (e >= 0.5)]
    assert len(lo) > 15 and len(hi) > 15
    assert np.median(lo) == pytest.approx(0.2, abs=0.07)
    assert np.median(hi) == pytest.approx(0.8, abs=0.07)
    assert np.median(s[double]) == pytest.approx(0.5, abs=0.1)
    # and the live table still equals the batch on the simulated stream
    ref, pairs, _ = batch_table(macro, micro, ch, setup=setup, L=30, m=10, T=5e-4)
    np.testing.assert_array_equal(t["First Photon"], pairs[:, 0])
    np.testing.assert_array_equal(t["S delayed red (photons) | 2048-4096"],
                                  ref["S delayed red (photons) | 2048-4096"])
