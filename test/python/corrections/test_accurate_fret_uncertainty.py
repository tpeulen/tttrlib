# SPDX-License-Identifier: BSD-3-Clause
"""A/B of accurate_fret, its uncertainties and distances, and the bootstrap.

Reference: `afret_reference/uncertainty.py` (numpy transcription of chisurf).
The bootstrap is compared by handing tttrlib chisurf's own resampling draws
(`bootstrap_indices`), so the two see identical resamples; without them
tttrlib draws with its own generator, which is checked for reproducibility.
"""
import numpy as np
import pytest

import tttrlib
from afret_reference import auto as ref_auto
from afret_reference import calibrate as ref_cal
from afret_reference import live
from afret_reference import uncertainty as ref
from test_accurate_fret_calibrate import _lifetimes, _line
from test_accurate_fret_populations import _alex_bursts

TOL = dict(rtol=1e-12, atol=1e-13)
FACTORS = dict(gamma=0.82, alpha=0.07, beta=0.95, delta=0.05, bg_dd=0.6, bg_da=0.4, bg_aa=0.2,
               r0=54.0)


def test_efficiency_uncertainty():
    rng = np.random.default_rng(0)
    e = rng.uniform(-0.1, 1.1, 300)
    dd = rng.uniform(-1, 100, 300)
    dd[:3] = 0
    aa = rng.uniform(0, 80, 300)
    for kw in (dict(), dict(gamma=0.8, sigma_gamma=0.05, sigma_alpha=0.01, sigma_delta=0.02),
               dict(gamma=0.0, sigma_gamma=0.1, sigma_statistical=rng.uniform(0, 0.1, 300))):
        for f_aa in (None, aa):
            got = tttrlib.efficiency_uncertainty(e, dd, f_aa, **kw)
            want = ref.efficiency_uncertainty(e, dd, f_aa, **kw)
            for key in ("total", "systematic", "statistical"):
                np.testing.assert_allclose(got[key], want[key], **TOL)
            for key in ("gamma", "alpha", "delta"):
                np.testing.assert_allclose(got["terms"][key], want["terms"][key], **TOL)


def test_distance_from_efficiency():
    e = np.array([-0.2, 0.0, 1e-4, 0.1, 0.5, 0.93, 1.0, np.nan])
    for kw in (dict(), dict(sigma_efficiency=0.02, sigma_r0=2.0), dict(sigma_r0=1.0)):
        got = tttrlib.distance_from_efficiency(e, 52.0, **kw)
        want = ref.distance_from_efficiency(e, 52.0, **kw)
        np.testing.assert_allclose(got["distance"], want["distance"], **TOL)
        np.testing.assert_allclose(got["sigma"], want["sigma"], **TOL)


@pytest.mark.parametrize("variant", ["plain", "labels", "no_alex", "lifetime", "nan_unc"])
def test_accurate_fret(variant):
    dd, da, aa = _alex_bursts(4)
    kw = dict(factors=FACTORS, uncertainties={"gamma": 0.05, "alpha": 0.01, "delta": 0.02, "r0": 2.0})
    if variant == "labels":
        kw["labels"] = np.repeat([-1, 0, 1, 2], [150, 120, 250, 300])
    if variant == "no_alex":
        aa = None
    if variant == "nan_unc":
        kw["uncertainties"] = {"gamma": float("nan"), "alpha": None}
    got_kw, want_kw = dict(kw), dict(kw)
    if variant == "lifetime":
        tau = _lifetimes(dd, da)
        tau[:5] = np.nan
        lt, le = _line()
        got_kw.update(tau_f=tau, line=(lt, le), labels=kw.get("labels"))
        want_kw.update(tau_f=tau, line=ref_cal.Line(lt, le))
    got = tttrlib.accurate_fret(dd, da, aa, **got_kw)
    want = ref.accurate_fret(dd, da, aa, **want_kw)
    for key in ("E", "fc", "sigma_E", "sigma_E_systematic", "distance", "sigma_distance"):
        np.testing.assert_allclose(got[key], want[key], **TOL, err_msg=key)
    assert (got["S"] is None) == (want["S"] is None)
    assert (got["deviation"] is None) == (want["deviation"] is None)
    if want["deviation"] is not None:
        np.testing.assert_allclose(got["deviation"], want["deviation"], **TOL)
    assert len(got["populations"]) == len(want["populations"])
    for p, q in zip(got["populations"], want["populations"]):
        assert p.keys() == q.keys()
        for key in q:
            np.testing.assert_allclose(p[key], q[key], **TOL, err_msg=key)


@pytest.mark.parametrize("seed", [0, 3])
@pytest.mark.parametrize("case", [dict(), dict(lifetimes=True, fret=((0.45, 400),)),
                                  dict(priors={"gamma": (0.9, 0.1), "delta": (0.04, 0.01)})])
def test_auto_calibrate_with_bootstrap(seed, case):
    c = dict(case)
    burst_kw = {"fret": c.pop("fret")} if "fret" in c else {}
    dd, da, aa = _alex_bursts(seed, **burst_kw)
    tau = _lifetimes(dd, da, seed=seed) if c.pop("lifetimes", False) else None
    priors = c.pop("priors", {})
    lt, le = _line()
    opts = dict(line=(lt, le), n_bootstrap=25, seed=11,
                bootstrap_indices=ref.draw_like_chisurf(11))
    got = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa, "tau_f": tau},
                                 {"priors": priors}, opts)
    want = ref_auto.auto_calibrate(dd, da, aa, calib=ref_cal.Calib(priors=priors), tau_f=tau,
                                   line=ref_cal.Line(lt, le), n_bootstrap=25, seed=11,
                                   bootstrap=ref.bootstrap, final=ref.final_populations)
    for key in ("gamma", "alpha", "beta", "delta"):
        np.testing.assert_allclose(got["factors"][key], want["factors"][key], rtol=1e-10)
    for key in ("gamma", "alpha", "beta", "delta"):
        np.testing.assert_allclose(got["uncertainties"][key], want["uncertainties"][key],
                                   rtol=1e-9, err_msg=key)
    assert got["messages"] == want["messages"]
    assert len(got["populations"]) == len(want["populations"])
    for p, q in zip(got["populations"], want["populations"]):
        for key in q:
            np.testing.assert_allclose(p[key], q[key], rtol=1e-10, atol=1e-12, err_msg=key)


def test_bootstrap_with_tttrlib_generator_is_reproducible():
    dd, da, aa = _alex_bursts(1)
    run = lambda seed: tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa}, None,
                                              dict(n_bootstrap=30, seed=seed))["uncertainties"]
    a, b, c = run(5), run(5), run(6)
    keys = ("alpha", "delta", "gamma", "beta")
    assert all(a[k] == b[k] for k in keys) and any(a[k] != c[k] for k in keys)
    for key in keys:
        assert 0 < a[key] < 0.5


def test_refine_gamma():
    dd, da, aa = _alex_bursts(2)
    labels = np.repeat([0, 1, 2, 1], [150, 120, 250, 300])
    rng = np.random.default_rng(3)
    idx = np.stack([rng.integers(0, dd.size, dd.size) for _ in range(40)])
    got = tttrlib.refine_gamma(dd, da, aa, labels, alpha=0.07, delta=0.05, prior=(0.9, 0.1),
                               n_bootstrap=40, indices=idx)
    want = ref.refine_gamma(dd, da, aa, labels, alpha=0.07, delta=0.05, prior=(0.9, 0.1),
                            n_bootstrap=40, seed=3)
    for key in want:
        np.testing.assert_allclose(got[key], want[key], rtol=1e-10, err_msg=key)
    fixed = tttrlib.refine_gamma(dd, da, aa, labels, alpha=0.07, delta=0.05, data_sigma=0.02)
    assert fixed["data_sigma"] == 0.02 and fixed["gamma_posterior"] == fixed["gamma_data"]


def test_transcription_matches_chisurf():
    acc = live.load("core.fluorescence.fret.accurate")
    if acc is None:
        pytest.skip("chisurf's accurate.py is gone; the transcription stands on its own")
    dd, da, aa = _alex_bursts(4)
    unc = {"gamma": 0.05, "alpha": 0.01, "delta": 0.02, "r0": 2.0}
    labels = np.repeat([-1, 0, 1, 2], [150, 120, 250, 300])
    a = acc.accurate_fret(dd, da, aa, calibration=FACTORS, uncertainties=unc, labels=labels)
    b = ref.accurate_fret(dd, da, aa, factors=FACTORS, uncertainties=unc, labels=labels)
    # chisurf's E came through the C++ three-cube step (FMA): a few last bits
    for key in ("E", "sigma_E", "distance", "sigma_distance"):
        np.testing.assert_allclose(a[key], b[key], rtol=1e-13, atol=1e-14)
    assert [p["n"] for p in a["populations"]] == [p["n"] for p in b["populations"]]
    r = acc.auto_calibrate(dd, da, aa, n_bootstrap=20, seed=4)
    w = ref_auto.auto_calibrate(dd, da, aa, n_bootstrap=20, seed=4, bootstrap=ref.bootstrap,
                                final=ref.final_populations)
    for key in ("gamma", "alpha", "beta", "delta"):
        assert r.uncertainties[key] == pytest.approx(w["uncertainties"][key], rel=1e-12)
