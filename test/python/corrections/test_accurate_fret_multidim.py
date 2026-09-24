# SPDX-License-Identifier: BSD-3-Clause
"""Multidimensional gating (S, E, lifetimes, anisotropies) against ground truth.

Not an A/B: chisurf gated on S alone. Synthetic MFD/PIE bursts with known
species, factors and lifetimes; the gating and the lifetime route must find
them. `mfd_bursts` is shared with the species-specific tests.
"""
import numpy as np
import pytest

import tttrlib

TAU_D0, TAU_A0 = 4.0, 3.0
ALPHA, DELTA, BETA = 0.07, 0.05, 0.9


def mfd_bursts(seed=0, fret=((0.3, 1.0, 1200), (0.7, 1.0, 1200)), n_do=500, n_ao=400,
               tau_noise=0.15, dynamic=None):
    """Counts and lifetimes of donor-only, acceptor-only and FRET species.

    ``fret`` holds ``(E, gamma_s, n)`` per species: a species whose acceptor
    quantum yield differs has its own gamma and, with the radiative rate
    unchanged, an acceptor lifetime scaled by the same ratio. ``dynamic``
    maps a species index to a donor lifetime off the static line. Returns a
    dict of columns plus ``"truth"`` (species index per burst: -2 donor-only,
    -1 acceptor-only).
    """
    rng = np.random.default_rng(seed)
    cols = {k: [] for k in ("i_dd", "i_da", "i_aa", "tau_d", "tau_a", "r_d", "truth")}

    def add(dd, da, aa, tau_d, tau_a, r_d, label):
        n = len(dd)
        cols["i_dd"].append(rng.poisson(dd)), cols["i_da"].append(rng.poisson(da))
        cols["i_aa"].append(rng.poisson(aa))
        cols["tau_d"].append(tau_d + rng.normal(0, tau_noise, n))
        cols["tau_a"].append(tau_a + rng.normal(0, tau_noise, n))
        cols["r_d"].append(r_d + rng.normal(0, 0.02, n))
        cols["truth"].append(np.full(n, label))

    tot = rng.uniform(60, 200, n_do)
    add(tot, ALPHA * tot, np.full(n_do, 0.3), np.full(n_do, TAU_D0), np.full(n_do, np.nan), 0.1, -2)
    tot = rng.uniform(60, 200, n_ao)
    add(np.full(n_ao, 0.3), DELTA * tot * BETA, tot * BETA, np.full(n_ao, np.nan),
        np.full(n_ao, TAU_A0), 0.2, -1)
    for s, (e, g, n) in enumerate(fret):
        tot = rng.uniform(60, 200, n)
        f_dd, f_da, f_aa = tot * (1 - e) / g, tot * e, tot * BETA
        tau_d = (dynamic or {}).get(s, TAU_D0 * (1 - e))
        add(f_dd, f_da + ALPHA * f_dd + DELTA * f_aa, f_aa, np.full(n, tau_d),
            np.full(n, TAU_A0 * g), 0.1, s)
    out = {k: np.concatenate(v).astype(float) for k, v in cols.items()}
    out["truth"] = out["truth"].astype(int)
    return out


def _purity(mask, truth, label):
    return np.count_nonzero(truth[mask] == label) / max(np.count_nonzero(mask), 1)


def test_nd_mixture_recovers_components():
    rng = np.random.default_rng(1)
    a = rng.normal([0.2, 1.0], [0.05, 0.2], (400, 2))
    b = rng.normal([0.7, 3.0], [0.05, 0.2], (600, 2))
    x = np.vstack([a, b])
    x[3] = np.nan
    fit = tttrlib.best_gaussian_mixture_nd(x)
    assert fit["n_components"] == 2
    np.testing.assert_allclose(fit["means"], [[0.2, 1.0], [0.7, 3.0]], atol=0.03)
    np.testing.assert_allclose(fit["weights"], [0.4, 0.6], atol=0.01)
    assert fit["labels"][3] == -1 and np.isnan(fit["responsibilities"][3]).all()


@pytest.mark.parametrize("method", ["hdbscan", "gmm"])
def test_nd_gating_uses_lifetimes(method):
    c = mfd_bursts(0)
    dims = ["S", "E", "tau_d", "tau_a"]
    r = tttrlib.auto_calibrate(c, None, dict(dimensions=dims, population_method=method))
    split, truth = r["split"], c["truth"]
    assert split["method"] == {"gmm": "mixture_nd"}.get(method, method)
    assert split["components"]["dimensions"] == dims
    assert _purity(split["donor_only"], truth, -2) > 0.99
    assert _purity(split["acceptor_only"], truth, -1) > 0.99
    assert split["counts"]["fret_populations"] == 2
    probs = split["fret_probabilities"]
    assert probs.shape == (truth.size, 2)
    np.testing.assert_allclose(probs[split["fret"]].sum(axis=1), 1.0, atol=1e-12)
    assert r["tau_d0"] == pytest.approx(TAU_D0, abs=0.05)
    assert r["tau_a"] == pytest.approx(TAU_A0, abs=0.05)
    f = r["factors"]
    assert f["alpha"] == pytest.approx(ALPHA, abs=0.005)
    assert f["delta"] == pytest.approx(DELTA, abs=0.005)
    assert f["gamma"] == pytest.approx(1.0, rel=0.05)
    assert f["beta"] == pytest.approx(BETA, rel=0.05)


@pytest.mark.parametrize("method", ["hdbscan", pytest.param("gmm", marks=pytest.mark.slow)])
def test_lifetime_splits_what_s_and_e_cannot(method):
    # two species at the same E, one of them off the static line (dynamic). The
    # lifetime route would read the dynamic one as a different gamma, and the E-S
    # line has one E only, so gamma stays put ("es") and only the gating is tested
    c = mfd_bursts(2, fret=((0.5, 1.0, 1500), (0.5, 1.0, 1500)), dynamic={1: 3.2})
    s_only = tttrlib.auto_calibrate(c)
    nd = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E", "tau_d"], gamma_source="es",
                                                  population_method=method))
    assert s_only["split"]["counts"]["fret_populations"] == 1
    assert nd["split"]["counts"]["fret_populations"] == 2
    labels = nd["split"]["fret_labels"]
    for s in (0, 1):
        members = labels[c["truth"] == s]
        assert np.mean(members == np.bincount(members[members >= 0]).argmax()) > 0.95


def test_no_alex_gating_with_donor_lifetime():
    c = mfd_bursts(3)
    keep = c["truth"] != -1  # without ALEX the acceptor-only bursts carry no signal
    cols = {k: v[keep] for k, v in c.items() if k != "i_aa"}
    r = tttrlib.auto_calibrate(cols, None, dict(dimensions=["E", "tau_d"]))
    assert _purity(r["split"]["donor_only"], cols["truth"], -2) > 0.98
    assert r["factors"]["alpha"] == pytest.approx(ALPHA, abs=0.01)
    assert r["tau_d0"] == pytest.approx(TAU_D0, abs=0.05)


@pytest.mark.parametrize("method", ["hdbscan", "gmm"])
def test_lifetime_gamma_with_the_no_linker_line(method):
    c = mfd_bursts(4, fret=((0.4, 0.7, 2500),))
    r = tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "E", "tau_d"], population_method=method))
    assert np.isnan(r["gamma_estimates"]["es"])
    assert r["gamma_estimates"]["lifetime"] == pytest.approx(0.7, rel=0.05)
    assert r["factors"]["gamma"] == pytest.approx(0.7, rel=0.05)
    assert any("no-linker line" in m for m in r["messages"])


def test_declared_dimension_without_column_raises():
    c = mfd_bursts(0)
    del c["tau_a"]
    with pytest.raises(ValueError):
        tttrlib.auto_calibrate(c, None, dict(dimensions=["S", "tau_a"]))
