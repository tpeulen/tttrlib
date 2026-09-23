# SPDX-License-Identifier: BSD-3-Clause
"""Species-specific gamma and its model selection, against ground truth.

Not an A/B: chisurf had one global factor set. Synthetic bursts where each
FRET species has its own gamma (acceptor quantum yield) must come back with
those gammas; a shared-gamma sample must not grow species factors; without
lifetimes the species model is not identifiable and never selected.
"""
import numpy as np
import pytest

import tttrlib
from test_accurate_fret_multidim import BETA, mfd_bursts

SPECIES = ((0.3, 0.6, 2000), (0.7, 1.2, 2000))
SHARED = ((0.3, 1.0, 2000), (0.7, 1.0, 2000))


def _run(fret, dims, seed=5, **opts):
    c = mfd_bursts(seed, fret=fret)
    return c, tttrlib.auto_calibrate(c, None, dict(opts, dimensions=dims))


@pytest.mark.parametrize("dims", [None, ["S", "E", "tau_d"]])
@pytest.mark.parametrize("seed", [5, 6])
def test_species_gamma_is_recovered(dims, seed):
    c, r = _run(SPECIES, dims, seed)
    sp = r["species"]
    ms = sp["model_selection"]
    assert ms["identifiable"] and ms["selected"] == "species"
    assert ms["bic_species"] < ms["bic_shared"]
    g = sp["factors"]["gamma"]
    assert not g["pooled"] and len(g["values"]) == len(sp["labels"]) == 2
    np.testing.assert_allclose(g["values"], [0.6, 1.2], rtol=0.08)
    assert all(s > 0 for s in g["sigma"])
    for truth, (e, _, _) in enumerate(SPECIES):
        assert np.mean(sp["E"][c["truth"] == truth]) == pytest.approx(e, abs=0.02)
    assert sp["factors"]["beta"]["global"] == pytest.approx(BETA, rel=0.05)
    assert sp["factors"]["alpha"]["pooled"] and sp["factors"]["delta"]["pooled"]


@pytest.mark.parametrize("dims", [None, ["S", "E", "tau_d"]])
@pytest.mark.parametrize("seed", [5, 7])
def test_shared_gamma_is_not_split(dims, seed):
    c, r = _run(SHARED, dims, seed)
    sp = r["species"]
    assert sp["model_selection"]["selected"] == "shared"
    g = sp["factors"]["gamma"]
    assert g["pooled"] and g["values"] == [r["factors"]["gamma"]] * len(sp["labels"])
    assert r["factors"]["gamma"] == pytest.approx(1.0, rel=0.05)


def test_without_lifetimes_the_species_model_is_not_identifiable():
    c = mfd_bursts(5, fret=SPECIES)
    del c["tau_d"]
    r = tttrlib.auto_calibrate(c)
    ms = r["species"]["model_selection"]
    assert not ms["identifiable"] and ms["selected"] == "shared"
    assert np.isnan(ms["bic_species"])


def test_assignment_table_shape():
    c, r = _run(SPECIES, ["S", "E", "tau_d"])
    sp = r["species"]
    a = sp["assignment"]
    assert a.shape == (c["i_dd"].size, len(sp["labels"]))
    fret = r["split"]["fret"]
    np.testing.assert_allclose(a[fret].sum(axis=1), 1.0, atol=1e-12)
    assert np.all(a[~fret] == 0)
    assert np.array_equal(sp["label"], r["split"]["fret_labels"])
    assert [p["name"] for p in sp["populations"]] == sp["names"]
    assert sp["populations"][0]["tau_d"] == pytest.approx(4.0 * 0.7, abs=0.05)


def test_acceptor_lifetime_tracks_the_species_quantum_yield():
    c, r = _run(SPECIES, ["S", "E", "tau_d", "tau_a"])
    pops = r["species"]["populations"]
    # tau_A scales with phi_A like gamma does in this model: 3.0 * gamma_s
    assert [p["tau_a"] for p in pops] == pytest.approx([1.8, 3.6], abs=0.05)
    assert r["tau_a"] == pytest.approx(3.0, abs=0.05)


def test_cal1_measurement_keeps_shared_factors(cal1_columns):
    dd, da, aa = cal1_columns
    r = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa})
    ms = r["species"]["model_selection"]
    assert ms["selected"] == "shared" and not ms["identifiable"]
    assert r["species"]["factors"]["gamma"]["values"] == [r["factors"]["gamma"]] * 3


from test_accurate_fret_calibrate import cal1_columns  # noqa: E402,F401  (fixture)
