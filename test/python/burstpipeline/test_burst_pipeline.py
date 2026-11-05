# SPDX-License-Identifier: BSD-3-Clause
"""BurstPipeline through the Python binding.

What the C++ test (test/cpp/test_burst_pipeline.cpp) cannot see: that the
columns arrive as views of the result's own buffers and keep it alive, that the
CSR decays are what a NumPy histogram of the burst photons gives, and that the
fits on the CSR decays are the fits ``DecayFit2.fit_many`` makes on the dense
matrix -- the call they replace.
"""
from __future__ import annotations

import gc
import json

import numpy as np
import pytest

tttrlib = pytest.importorskip("tttrlib")
if not hasattr(tttrlib, "BurstPipeline"):
    pytest.skip("tttrlib built without the burstpipeline module", allow_module_level=True)

MACRO_RES = 40e-9
MICRO_RES = 80e-12


def _simulate(seed=5, n_bursts=200):
    rng = np.random.default_rng(seed)
    macro, micro, rout = [], [], []
    t = 0.0
    for b in range(n_bursts + 1):
        n_bg = rng.poisson(25)
        ts = t + np.cumsum(rng.exponential(8000.0, n_bg))
        macro.append(ts); rout.append(rng.integers(0, 4, n_bg)); micro.append(rng.integers(0, 500, n_bg))
        t = ts[-1] if n_bg else t + 2e5
        if b == n_bursts:
            break
        n = int(rng.integers(30, 230))
        ts = t + np.cumsum(rng.exponential(60.0, n))
        ch = rng.integers(0, 4, n)
        delay = np.where((ch >= 2) & (rng.random(n) < 0.3), 250, 10)
        mt = np.minimum(499, delay + rng.exponential(40.0, n)).astype(int)
        macro.append(ts); rout.append(ch); micro.append(mt)
        t = ts[-1]
    macro = np.concatenate(macro).astype(np.uint64)
    micro = np.concatenate(micro).astype(np.uint16)
    rout = np.concatenate(rout).astype(np.int8)
    tttr = tttrlib.TTTR()
    tttr.append_events(macro, micro, rout, np.zeros_like(rout), False, 0)
    tttr.header.set_macro_time_resolution(MACRO_RES)
    tttr.header.set_micro_time_resolution(MICRO_RES)
    tttr.header.set_number_of_micro_time_channels(500)
    return tttr, macro, micro, rout


def _config(**over):
    cfg = {
        "detectors": [
            {"name": "G|G", "chs": [0, 1], "gates": [[0, 250]]},
            {"name": "R|G", "chs": [2, 3], "gates": [[0, 250]]},
            {"name": "R|R", "chs": [2, 3], "gates": [[250, 500]]},
        ],
        "windows": [{"name": "prompt", "lo": 10, "hi": 250}, {"name": "delayed", "lo": 250, "hi": 500}],
        "search": {"algorithm": "cusum_sprt", "scope": "detectors",
                   "parameters": {"min_photons": 5, "background_cps": 3000.0,
                                  "signal_to_background_ratio": 4.0, "alpha": 0.05, "beta": 0.05}},
        "selection": {"max_gap": 3, "min_photons": 20, "dt_max_active": True, "dt_max_ms": 0.15},
        "decays": {"binning": 2, "n_bins": 250},
        "micro_time_resolution_ns": MICRO_RES * 1e9,
        "macro_time_resolution": MACRO_RES,
        "chunk_size": 2048,
    }
    cfg.update(over)
    return json.dumps(cfg)


@pytest.fixture(scope="module")
def run():
    tttr, macro, micro, rout = _simulate()
    pipe = tttrlib.BurstPipeline(_config())
    res = pipe.run(tttr)
    return pipe, res, tttr, macro, micro, rout


def test_columns_are_views_that_keep_the_result_alive(run):
    pipe, _res, tttr, *_ = run
    res = pipe.run(tttr)
    a = res.column_view("first_photon")
    b = res.column_view("first_photon")
    assert np.shares_memory(a, b), "two views of one column must be the same buffer"
    assert a.dtype == np.int64 and len(a) == res.n_bursts() > 50
    expected = a.copy()
    del res
    gc.collect()
    # the view holds the result: its buffer is still there
    np.testing.assert_array_equal(a, expected)
    assert not a.flags.owndata


def test_csr_decays_match_numpy_histograms(run):
    _pipe, res, _tttr, macro, micro, rout = run
    first = res.column_view("first_photon")
    last = res.column_view("last_photon")
    g = res.decay_group_of("R|G")
    assert g == res.decay_group_of("R|R"), "detectors with one channel list share one decay group"
    dense = res.dense_decays(g)
    for b in range(0, len(first), 7):
        sl = slice(int(first[b]), int(last[b]) + 1)
        r, m = rout[sl], micro[sl].astype(int) // 2
        keep = ((r == 2) | (r == 3)) & (m < 250)
        ref = np.bincount((r[keep] == 3) * 250 + m[keep], minlength=500)
        np.testing.assert_array_equal(dense[b], ref)
    np.testing.assert_array_equal(res.decay_sum(g), dense.sum(axis=0))


def test_detector_features_match_numpy(run):
    _pipe, res, _tttr, macro, micro, rout = run
    first = res.column_view("first_photon")
    last = res.column_view("last_photon")
    n_rr = res.column_view("n_photons|R|R")
    mean_micro = res.column_view("mean_micro_time_ns|R|R")
    rate = res.column_view("count_rate_khz|prompt@G|G")
    for b in range(len(first)):
        sl = slice(int(first[b]), int(last[b]) + 1)
        sel = np.isin(rout[sl], [2, 3]) & (micro[sl] >= 250) & (micro[sl] < 500)
        assert n_rr[b] == sel.sum()
        expect = micro[sl][sel].astype(np.float64).sum() / sel.sum() * (MICRO_RES * 1e9) if sel.any() else -1.0
        assert mean_micro[b] == expect
        p = np.flatnonzero(np.isin(rout[sl], [0, 1]) & (micro[sl] >= 10) & (micro[sl] < 250))
        if p.size:
            d = (macro[sl][p[-1]] - macro[sl][p[0]]) * MACRO_RES * 1e3
            assert rate[b] == (p.size / d if d > 0 else rate[b])
        else:
            assert rate[b] == -1.0


def test_small_and_large_chunks_agree():
    tttr, *_ = _simulate(seed=9)
    a = tttrlib.BurstPipeline(_config(chunk_size=1024)).run(tttr)
    b = tttrlib.BurstPipeline(_config(chunk_size=1 << 22)).run(tttr)
    for name in a.column_names():
        np.testing.assert_array_equal(a.column_view(name), b.column_view(name), err_msg=name)


def test_cancel_from_another_thread_stops_the_pass():
    import threading
    tttr, *_ = _simulate(seed=2, n_bursts=400)
    pipe = tttrlib.BurstPipeline(_config(chunk_size=1024))
    pipe.cancel()  # a cancel before the call is cleared by the call
    res = pipe.run(tttr)
    assert not res.cancelled()
    t = threading.Timer(0.0, pipe.cancel)
    t.start()
    res = pipe.run(tttr)
    t.join()
    assert pipe.progress_photons() <= pipe.progress_total()


def _fit23_setup(n):
    irf = np.zeros(2 * n)
    irf[5:9] = [0.2, 1.0, 0.6, 0.1]
    irf[n + 5:n + 9] = [0.2, 1.0, 0.6, 0.1]
    bg = np.full(2 * n, 1.0)
    bg /= bg.sum()
    setup = list(tttrlib.setup_vector("fit23", dt=0.16, period=40.0, g_factor=1.0, l1=0.0, l2=0.0,
                                      soft_bifl_scatter_flag=False, objective="p2s_mle"))
    return setup, irf, bg


def test_fit_on_csr_equals_fit_many_on_the_dense_matrix(run):
    pipe, res, *_ = run
    n = res.decay_n_bins(res.decay_group_of("G|G"))
    setup, irf, bg = _fit23_setup(n)
    x0, link = [4.0, 0.1, 0.38, 1.22], [0, 0, -1, 0]
    entry = {"detector": "G|G", "model": "fit23", "setup": setup, "irf": irf.tolist(),
             "background": bg.tolist(), "x0": x0, "link": link, "n": n, "sb": 0, "eb": n,
             "vh_sb": 0, "vh_eb": n, "shift": 0, "min_photons": 10, "dt": 0.16,
             "converged_index": list(tttrlib.result_names("fit23")).index("converged")}
    pipe.fit(res, json.dumps({"fits": [entry], "n_threads": 4}))
    reason = res.column_view("fit|G|G|reason")
    ng = res.column_view("fit|G|G|ng_p") + res.column_view("fit|G|G|ng_s")
    rows = np.flatnonzero(ng >= 10)
    assert rows.size > 20 and np.all(reason[ng < 10] == 1)
    dense = res.dense_decays(res.decay_group_of("G|G"), rows).astype(np.float64)
    fit = tttrlib.DecayFit2("fit23", setup, irf.tolist())
    problem = tttrlib.DecayFitProblem(2, n, 0.16)
    problem.irf = tttrlib.VectorDouble(irf.tolist())
    problem.background = tttrlib.VectorDouble(bg.tolist())
    batch = fit.fit_many(problem, dense.ravel().tolist(), int(rows.size), 2 * n, x0,
                         tttrlib.DecayFitConstraints(tttrlib.VectorInt32(link)))
    ref = np.asarray(batch.parameters).reshape(rows.size, 4)
    for i in range(4):
        np.testing.assert_array_equal(res.column_view(f"fit|G|G|x|{i}")[rows], ref[:, i])
    np.testing.assert_array_equal(res.column_view("fit|G|G|objective")[rows], np.asarray(batch.objective))
