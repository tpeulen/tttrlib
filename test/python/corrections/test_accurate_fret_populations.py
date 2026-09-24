# SPDX-License-Identifier: BSD-3-Clause
"""A/B of the AccurateFret population gating and the alpha/delta estimators.

Reference: `afret_reference/populations.py`, the numpy transcription of
chisurf's gating. The EM mixture agrees to 1e-10 (the one matmul sums in BLAS
order in numpy), masks and labels exactly, the estimators to 1e-12.
"""
import numpy as np
import pytest

import tttrlib
from afret_reference import live
from afret_reference import populations as ref

MIX = dict(rtol=1e-10, atol=1e-10)


def _alex_bursts(seed=0, n_do=150, n_ao=120, fret=((0.25, 250), (0.7, 300)), gamma=0.8,
                 beta=0.9, alpha=0.07, delta=0.05):
    """Photon counts of donor-only, acceptor-only and FRET bursts (true factors known)."""
    rng = np.random.default_rng(seed)
    dd, da, aa = [], [], []

    def add(n, e):
        tot = rng.uniform(40, 160, n)
        f_dd = tot * (1 - e) / gamma
        f_da = tot * e
        f_aa = tot / beta
        dd.append(rng.poisson(f_dd))
        da.append(rng.poisson(f_da + alpha * f_dd + delta * f_aa))
        aa.append(rng.poisson(f_aa))

    tot = rng.uniform(40, 160, n_do)
    dd.append(rng.poisson(tot)), da.append(rng.poisson(alpha * tot)), aa.append(rng.poisson(0.3, n_do))
    tot = rng.uniform(40, 160, n_ao)
    dd.append(rng.poisson(0.3, n_ao)), da.append(rng.poisson(delta * tot)), aa.append(rng.poisson(tot))
    for e, n in fret:
        add(n, e)
    return [np.concatenate(v).astype(float) for v in (dd, da, aa)]


def _es(dd, da, aa):
    r = tttrlib.apparent_es(dd, da, aa)
    return r["E"], r["S"]


def _same_mixture(got, want):
    for key in ("weights", "means", "sigmas", "responsibilities"):
        np.testing.assert_allclose(got[key], want[key], **MIX, err_msg=key)
    np.testing.assert_array_equal(got["labels"], want["labels"])
    np.testing.assert_allclose(got["log_likelihood"], want["log_likelihood"], rtol=1e-11)
    np.testing.assert_allclose(got["bic"], want["bic"], rtol=1e-11)


@pytest.mark.parametrize("seed", [0, 1, 2])
@pytest.mark.parametrize("k", [1, 2, 3, 4])
def test_gaussian_mixture_1d(seed, k):
    dd, da, aa = _alex_bursts(seed)
    _, s = _es(dd, da, aa)
    s[:4] = np.nan
    _same_mixture(tttrlib.gaussian_mixture_1d(s, k), ref.gaussian_mixture_1d(s, k))


@pytest.mark.parametrize("init", ["quantile", "range"])
def test_gaussian_mixture_1d_single_start(init):
    x = np.random.default_rng(5).normal(0.3, 0.1, 300)
    _same_mixture(tttrlib.gaussian_mixture_1d(x, 3, init=init),
                  ref.gaussian_mixture_1d(x, 3, init=init))


def test_gaussian_mixture_1d_errors():
    with pytest.raises(ValueError):
        tttrlib.gaussian_mixture_1d([np.nan, np.inf], 2)
    with pytest.raises(ValueError):
        tttrlib.gaussian_mixture_1d([0.1, 0.2], 3)


@pytest.mark.parametrize("seed", [0, 3])
def test_best_mixture(seed):
    dd, da, aa = _alex_bursts(seed)
    _, s = _es(dd, da, aa)
    got, want = tttrlib.best_gaussian_mixture_1d(s), ref.best_mixture(s)
    _same_mixture(got, want)
    assert list(got["bic_by_k"]) == list(want["bic_by_k"])
    np.testing.assert_allclose(list(got["bic_by_k"].values()), list(want["bic_by_k"].values()),
                               rtol=1e-11)


@pytest.mark.parametrize("seed", [0, 1, 2, 3])
@pytest.mark.parametrize("kwargs", [{}, dict(reference_sigma=0.0), dict(method="threshold"),
                                    dict(min_population=200)])
def test_classify_es_populations(seed, kwargs):
    dd, da, aa = _alex_bursts(seed)
    e, s = _es(dd, da, aa)
    got = tttrlib.classify_es_populations(s, e, **kwargs)
    want = ref.classify_es_populations(s, e, **kwargs)
    for key in ("donor_only", "acceptor_only", "fret", "fret_labels"):
        np.testing.assert_array_equal(got[key], want[key], err_msg=key)
    np.testing.assert_allclose(got["thresholds"], want["thresholds"], **MIX)
    assert got["method"] == want["method"]
    if want["components"]:
        np.testing.assert_allclose(got["components"]["means"], want["components"]["means"], **MIX)


def test_classify_finds_the_three_species():
    dd, da, aa = _alex_bursts(7)
    e, s = _es(dd, da, aa)
    c = tttrlib.classify_es_populations(s, e)["counts"]
    assert c["donor_only"] > 100 and c["acceptor_only"] > 80
    assert c["fret_populations"] == 2


@pytest.mark.parametrize("seed", [0, 4])
def test_split_fret_subpopulations(seed):
    rng = np.random.default_rng(seed)
    e = np.concatenate([rng.normal(0.2, 0.06, 200), rng.normal(0.65, 0.06, 150),
                        rng.normal(0.9, 0.03, 15), [np.nan]])
    np.testing.assert_array_equal(tttrlib.split_fret_subpopulations(e),
                                  ref.split_fret_subpopulations(e))


def test_reference_estimators():
    dd, da, aa = _alex_bursts(2)
    kw = dict(bg_dd=0.8, bg_da=0.6)
    assert tttrlib.leakage_from_donor_only(dd, da, **kw) == pytest.approx(
        ref.leakage_from_donor_only(dd, da, **kw), rel=1e-12, abs=1e-15)
    for i_dd, alpha in ((None, 0.0), (dd, 0.07), (dd, 0.0)):
        kw = dict(alpha=alpha, bg_dd=0.8, bg_da=0.6, bg_aa=0.3)
        assert tttrlib.direct_excitation_from_acceptor_only(da, aa, i_dd, **kw) == pytest.approx(
            ref.direct_excitation_from_acceptor_only(da, aa, i_dd, **kw), rel=1e-12, abs=1e-15)
    assert tttrlib.leakage_from_donor_only([2.0], [1.0], bg_dd=2.0) == 0.0


def test_transcription_matches_chisurf():
    acc = live.load("core.fluorescence.fret.accurate", "gaussian_mixture_1d", "classify_es_populations")
    cal = live.load("core.fluorescence.fret.calibration", "leakage_from_donor_only")
    if acc is None or cal is None:
        pytest.skip("chisurf's accurate.py is gone; the transcription stands on its own")
    dd, da, aa = _alex_bursts(1)
    e, s = _es(dd, da, aa)
    for k in (1, 2, 3):
        a, b = acc.gaussian_mixture_1d(s, k), ref.gaussian_mixture_1d(s, k)
        np.testing.assert_allclose(a["means"], b["means"], rtol=1e-12, atol=1e-12)
        np.testing.assert_array_equal(a["labels"], b["labels"])
    a, b = acc.classify_es_populations(s, e), ref.classify_es_populations(s, e)
    for key in ("donor_only", "acceptor_only", "fret", "fret_labels"):
        np.testing.assert_array_equal(getattr(a, key), b[key])
    assert cal.leakage_from_donor_only(dd, da, bg_dd=1.0) == ref.leakage_from_donor_only(
        dd, da, bg_dd=1.0)
    assert cal.direct_excitation_from_acceptor_only(da, aa, dd, alpha=0.1) == \
        ref.direct_excitation_from_acceptor_only(da, aa, dd, alpha=0.1)
