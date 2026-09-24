# SPDX-License-Identifier: BSD-3-Clause
"""Density-based population finding (HDBSCAN) and the outlier pre-cleaning.

Ground truth, not an A/B: synthetic MFD/PIE bursts with donor-only,
acceptor-only, two or three FRET species and background bursts that belong to
no species. HDBSCAN must find the species without being told how many there
are, leave the background bursts out of every class, and match or beat the
Gaussian mixture on the same data.
"""
import numpy as np
import pytest

import tttrlib
from test_accurate_fret_multidim import ALPHA, BETA, DELTA, TAU_A0, TAU_D0, mfd_bursts

THREE = ((0.2, 1.0, 1500), (0.5, 1.0, 1500), (0.8, 1.0, 1500))


def with_background(c, n=300, seed=11):
    """Append ``n`` bursts of pure background: few photons, no species (truth -3)."""
    rng = np.random.default_rng(seed)
    extra = {"i_dd": rng.poisson(6, n), "i_da": rng.poisson(6, n), "i_aa": rng.poisson(6, n),
             "tau_d": rng.uniform(0.2, 6.0, n), "tau_a": rng.uniform(0.2, 6.0, n),
             "r_d": rng.uniform(-0.1, 0.4, n), "truth": np.full(n, -3)}
    return {k: np.concatenate([c[k], np.asarray(extra[k], dtype=c[k].dtype)]) for k in c}


def _purity(mask, truth, label):
    return np.count_nonzero(truth[mask] == label) / max(np.count_nonzero(mask), 1)


def _label_purity(split, truth, n_species):
    labels = split["fret_labels"]
    out = []
    for s in range(n_species):
        members = labels[(truth == s) & (labels >= 0)]
        top = np.bincount(members).argmax()
        out.append(np.mean(truth[labels == top] == s))
    return out


def test_defaults_and_option_names():
    o = tttrlib.AutoCalibrateOptions()
    assert o.population_method == "hdbscan" and o.hdbscan_selection == "leaf"
    assert set(tttrlib.AFRET_POPULATION_METHODS) == {"hdbscan", "gmm"}
    with pytest.raises(ValueError):
        tttrlib.auto_calibrate(mfd_bursts(0), None, dict(dimensions=["S", "E"],
                                                         population_method="kmeans"))


@pytest.mark.parametrize("seed", [0, 1])
def test_hdbscan_finds_three_species_and_leaves_background_out(seed):
    c = with_background(mfd_bursts(seed, fret=THREE), seed=seed + 20)
    r = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E"]))
    split, truth = r["split"], c["truth"]
    assert split["method"] == "hdbscan"
    assert split["counts"]["fret_populations"] == 3
    assert _purity(split["donor_only"], truth, -2) > 0.98
    assert _purity(split["acceptor_only"], truth, -1) > 0.99
    assert min(_label_purity(split, truth, 3)) > 0.93
    # background bursts (S ~ 0.67, E ~ 0.5) overlap the middle species; the
    # mixture puts every one of them in a class, HDBSCAN most of them in noise
    background = truth == -3
    in_class = split["donor_only"] | split["acceptor_only"] | split["fret"]
    assert np.mean(in_class[background]) < 0.5
    assert split["noise"][background].mean() > 0.5
    f = r["factors"]
    assert f["alpha"] == pytest.approx(ALPHA, abs=0.005)
    assert f["delta"] == pytest.approx(DELTA, abs=0.005)
    assert f["gamma"] == pytest.approx(1.0, rel=0.05)
    assert f["beta"] == pytest.approx(BETA, rel=0.05)
    probs = split["fret_probabilities"]
    np.testing.assert_allclose(probs[split["fret"]].sum(axis=1), 1.0, atol=1e-12)
    assert not probs[split["noise"]].any()


def test_hdbscan_matches_or_beats_the_mixture():
    c = with_background(mfd_bursts(3, fret=THREE))
    truth = c["truth"]
    res = {m: tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E"], population_method=m))
           for m in ("hdbscan", "gmm")}
    hd, gm = res["hdbscan"], res["gmm"]
    assert gm["split"]["method"] == "mixture_nd" and gm["split"]["noise"] is None
    err = {m: abs(r["factors"]["gamma"] - 1.0) for m, r in res.items()}
    assert err["hdbscan"] <= err["gmm"] + 0.02
    assert _purity(hd["split"]["donor_only"], truth, -2) >= _purity(gm["split"]["donor_only"], truth, -2) - 0.005


def test_hdbscan_with_lifetimes_splits_equal_e():
    c = mfd_bursts(2, fret=((0.5, 1.0, 1500), (0.5, 1.0, 1500)), dynamic={1: 3.2})
    r = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E", "tau_d", "tau_a"],
                                             gamma_source="es"))
    split = r["split"]
    assert split["method"] == "hdbscan" and split["counts"]["fret_populations"] == 2
    assert min(_label_purity(split, c["truth"], 2)) > 0.95
    assert r["tau_d0"] == pytest.approx(TAU_D0, abs=0.05)
    assert r["tau_a"] == pytest.approx(TAU_A0, abs=0.05)


def test_hdbscan_species_gamma():
    c = with_background(mfd_bursts(5, fret=((0.3, 0.6, 2000), (0.7, 1.2, 2000))))
    r = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E", "tau_d"]))
    sp = r["species"]
    assert sp["model_selection"]["selected"] == "species"
    np.testing.assert_allclose(sp["factors"]["gamma"]["values"], [0.6, 1.2], rtol=0.08)
    # the P(FRET n) columns: probabilities of the FRET bursts, 0 outside
    a = sp["assignment"]
    fret = r["split"]["fret"]
    np.testing.assert_allclose(a[fret].sum(axis=1), 1.0, atol=1e-12)
    assert not a[~fret].any()
    # noise bursts are out of the estimation but still get corrected values
    noise = r["split"]["noise"]
    assert noise.any() and np.isfinite(sp["E"][noise]).all()


def test_subsampling_keeps_the_answer():
    c = mfd_bursts(8, fret=THREE)
    full = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E"], hdbscan_max_points=0))
    sub = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E"], hdbscan_max_points=1500))
    assert full["split"]["counts"]["fret_populations"] == sub["split"]["counts"]["fret_populations"] == 3
    for k in ("alpha", "delta", "gamma", "beta"):
        assert sub["factors"][k] == pytest.approx(full["factors"][k], rel=0.03, abs=0.003), k


def test_flag_dimension_outliers():
    x = {"S": [0.5, 1.5, -0.3, np.nan, 0.9], "tau_d": [1.0, 2.0, -1.0, np.inf, 1000.0]}
    out = tttrlib.flag_dimension_outliers(x, fence=0)
    assert out["by_dimension"]["S"]["range"] == 2 and out["by_dimension"]["tau_d"]["range"] == 3
    np.testing.assert_array_equal(out["mask"], [False, True, True, True, True])
    rng = np.random.default_rng(0)
    t = np.concatenate([rng.normal(3, 0.2, 500), [15.0, 18.0]])
    fenced = tttrlib.flag_dimension_outliers({"tau_d": t})
    assert fenced["by_dimension"]["tau_d"]["fence"] == 2 and fenced["n"] == 2
    # a minority population is not an outlier (a quartile fence would cut it)
    two = np.concatenate([rng.normal(2.4, 0.15, 2500), rng.normal(4.0, 0.15, 500)])
    assert tttrlib.flag_dimension_outliers({"tau_d": two})["n"] == 0


def _wild(c, seed=100, n_wild=60):
    """Bursts whose donor lifetime is absurd (a failed fit): 1e3-1e5 ns."""
    rng = np.random.default_rng(seed)
    hit = rng.choice(np.flatnonzero(c["truth"] >= 0), n_wild, replace=False)
    c["tau_d"][hit] = rng.uniform(1e3, 1e5, n_wild)
    return c, hit


@pytest.mark.parametrize("method", ["hdbscan", "gmm"])
def test_outliers_are_removed_before_scaling(method):
    c, hit = _wild(mfd_bursts(0))
    clean = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E", "tau_d"],
                                                 population_method=method))
    out = clean["outliers"]
    assert out["n"] == len(hit) and out["mask"][hit].all()
    tau = out["by_dimension"]["tau_d"]
    assert tau["fence"] + tau["range"] == len(hit) and tau["hi"] < 20
    split = clean["split"]
    assert not (split["fret"] | split["donor_only"] | split["acceptor_only"])[hit].any()
    if method == "hdbscan":
        assert not split["noise"][hit].any()  # outliers are not HDBSCAN noise
        assert split["counts"]["noise"] + split["counts"]["outlier"] == \
            np.count_nonzero(split["noise"] | split["outlier"])
    assert split["counts"]["fret_populations"] == 2
    assert min(_label_purity(split, c["truth"], 2)) > 0.95
    assert clean["tau_d0"] == pytest.approx(TAU_D0, abs=0.05)


@pytest.mark.slow
def test_without_pre_cleaning_wild_lifetimes_break_the_mixture():
    # two species at one E, told apart by the donor lifetime only: wild values
    # blow up the standardised lifetime axis of the mixture
    c, hit = _wild(mfd_bursts(2, fret=((0.5, 1.0, 1500), (0.5, 1.0, 1500)), dynamic={1: 3.2}),
                   n_wild=150)
    dims = ["S", "E", "tau_d"]
    opts = dict(dimensions=dims, gamma_source="es")
    raw = tttrlib.auto_calibrate(c, None, dict(opts, population_method="gmm", remove_outliers=False))
    assert raw["outliers"] is None
    classes = raw["split"]["donor_only"] | raw["split"]["acceptor_only"]
    assert raw["split"]["counts"]["fret_populations"] == 1 and classes[hit].any()
    for method in ("gmm", "hdbscan"):
        clean = tttrlib.auto_calibrate(c, None, dict(opts, population_method=method))
        split = clean["split"]
        assert split["counts"]["fret_populations"] == 2, method
        assert min(_label_purity(split, c["truth"], 2)) > 0.95, method
        assert not (split["donor_only"] | split["acceptor_only"] | split["fret"])[hit].any()
    # HDBSCAN's robust (IQR) scale is not stretched by them either; they end as noise
    hd = tttrlib.auto_calibrate(c, None, dict(opts, remove_outliers=False))
    assert hd["split"]["noise"][hit].all()
    assert hd["split"]["counts"]["fret_populations"] == 2
