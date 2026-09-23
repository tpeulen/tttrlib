# SPDX-License-Identifier: BSD-3-Clause
"""A/B of the AccurateFret E/S kernels against the chisurf reference.

The reference is `afret_reference/es.py`, a numpy transcription of chisurf's
`core/fluorescence/burst/es.py`; while chisurf still carries the original, the
transcription itself is checked against it. Closed forms agree to 1e-12.
"""
import numpy as np
import pytest

import tttrlib
from afret_reference import es as ref
from afret_reference import live

TOL = dict(rtol=1e-12, atol=1e-12)


def _bursts(seed=0, n=400):
    rng = np.random.default_rng(seed)
    dd = rng.poisson(rng.uniform(5, 200, n)).astype(float)
    da = rng.poisson(rng.uniform(0, 200, n)).astype(float)
    aa = rng.poisson(rng.uniform(0, 150, n)).astype(float)
    # the guards: zero denominators and over-subtracted bursts
    dd[:3] = 0.0
    da[:2] = 0.0
    aa[:1] = 0.0
    return dd, da, aa


FACTORS = [
    dict(),
    dict(gamma=0.73, alpha=0.061, beta=1.31, delta=0.084, bg_dd=1.2, bg_da=0.7, bg_aa=0.4),
    dict(gamma=1.9, alpha=0.3, beta=0.45, delta=0.25, bg_dd=4.0, bg_da=6.0, bg_aa=3.0),
]


@pytest.mark.parametrize("seed", [0, 1, 2])
@pytest.mark.parametrize("alex", [True, False])
def test_apparent_es(seed, alex):
    dd, da, aa = _bursts(seed)
    got = tttrlib.apparent_es(dd, da, aa if alex else None)
    want = ref.apparent_es(dd, da, aa if alex else None)
    np.testing.assert_allclose(got["E"], want["E"], **TOL)
    if alex:
        np.testing.assert_allclose(got["S"], want["S"], **TOL)
    else:
        assert got["S"] is None


@pytest.mark.parametrize("factors", FACTORS)
@pytest.mark.parametrize("alex", [True, False])
def test_corrected_es(factors, alex):
    dd, da, aa = _bursts(3)
    got = tttrlib.corrected_es(dd, da, aa if alex else None, **factors)
    want = ref.corrected_es(dd, da, aa if alex else None, **factors)
    for key in ("E", "fc") + (("S",) if alex else ()):
        np.testing.assert_allclose(got[key], want[key], **TOL, err_msg=key)
    assert (got["S"] is None) == (not alex)


def test_corrected_es_broadcasts_scalars_and_shapes():
    dd = np.arange(12.0).reshape(3, 4) + 5
    got = tttrlib.corrected_es(dd, 7.0, 3.0, gamma=0.8, alpha=0.1, delta=0.05)
    want = ref.corrected_es(dd, np.full_like(dd, 7.0), np.full_like(dd, 3.0),
                            gamma=0.8, alpha=0.1, delta=0.05)
    assert got["E"].shape == (3, 4)
    np.testing.assert_allclose(got["S"], want["S"], **TOL)


def test_corrected_es_rejects_ragged_input():
    with pytest.raises(ValueError):
        tttrlib.corrected_es(np.ones(3), np.ones(4), np.ones(3))


def _matrix_case(seed, n=3, bursts=(50,)):
    rng = np.random.default_rng(seed)
    inten = rng.uniform(1, 300, (n, n) + bursts)
    gamma = rng.uniform(0.5, 1.5, (n, n))
    alpha = rng.uniform(0.0, 0.1, (n, n))
    delta = rng.uniform(0.0, 0.1, (n, n))
    bg = rng.uniform(0.0, 3.0, (n, n))
    return inten, gamma, alpha, delta, bg


@pytest.mark.parametrize("seed", [0, 1])
@pytest.mark.parametrize("pairs", [None, [(0, 2), (0, 1), (1, 2)], [(1, 2)]])
def test_corrected_es_matrix(seed, pairs):
    inten, gamma, alpha, delta, bg = _matrix_case(seed)
    got = tttrlib.corrected_es_matrix(inten, gamma, alpha, delta, bg, pairs)
    want = ref.corrected_es_matrix(inten, gamma, alpha, delta, bg, pairs)
    assert list(got) == list(want)
    for key in want:
        for field in ("E", "fc"):
            np.testing.assert_allclose(got[key][field], want[key][field], **TOL)


def test_corrected_es_matrix_without_burst_axis_and_defaults():
    inten, gamma, alpha, _, _ = _matrix_case(4, n=4, bursts=())
    got = tttrlib.corrected_es_matrix(inten, gamma, alpha)
    want = ref.corrected_es_matrix(inten, gamma, alpha)
    for key in want:
        assert np.shape(got[key]["E"]) == np.shape(want[key]["E"])
        np.testing.assert_allclose(got[key]["E"], want[key]["E"], **TOL)


def _general_case(seed, n=3, bursts=(40,)):
    rng = np.random.default_rng(seed)
    exc = np.eye(n) + np.triu(rng.uniform(0.0, 0.2, (n, n)), 1)
    emis = np.eye(n) * rng.uniform(0.6, 1.4, n) + np.triu(rng.uniform(0.0, 0.15, (n, n)), 1)
    inten = rng.uniform(5, 300, (n, n) + bursts)
    bg = rng.uniform(0, 2, (n, n))
    return inten, exc, emis, bg


@pytest.mark.parametrize("seed", [0, 1])
@pytest.mark.parametrize("ridge", [0.0, 0.05])
def test_corrected_es_general_naive(seed, ridge):
    inten, exc, emis, bg = _general_case(seed)
    got = tttrlib.corrected_es_general(inten, exc, emis, background=bg, ridge=ridge)
    want = ref.corrected_es_general(inten, exc, emis, background=bg, ridge=ridge)
    assert list(got) == list(want)
    for key in want:
        for field in ("E", "fc"):
            np.testing.assert_allclose(got[key][field], want[key][field], rtol=1e-11, atol=1e-11)


@pytest.mark.parametrize("ridge", [0.0, 0.05])
def test_corrected_es_general_stable(ridge):
    pytest.importorskip("scipy")
    inten, exc, emis, bg = _general_case(5)
    got = tttrlib.corrected_es_general(inten, exc, emis, background=bg, unmix="nnls", ridge=ridge)
    want = ref.corrected_es_general(inten, exc, emis, background=bg, unmix="stable", ridge=ridge)
    for key in want:
        for field in ("E", "fc"):
            np.testing.assert_allclose(got[key][field], want[key][field], rtol=1e-9, atol=1e-9)


def test_corrected_es_general_reduces_to_two_colour():
    dd, da, aa = _bursts(6)
    g, a, d = 0.8, 0.07, 0.09
    inten = np.stack([np.stack([dd, da]), np.stack([np.zeros_like(aa), aa])])
    got = tttrlib.corrected_es_general(inten, [[1, d], [0, 1]], [[1, a], [0, g]])[(0, 1)]["E"]
    want = ref.corrected_es(dd, da, aa, gamma=g, alpha=a, delta=d)["E"]
    ok = (dd > 0) & (want > 0)
    np.testing.assert_allclose(got[ok], want[ok], rtol=1e-12, atol=1e-12)


def test_corrected_es_general_rejects_unknown_unmix():
    inten, exc, emis, _ = _general_case(0)
    with pytest.raises(ValueError):
        tttrlib.corrected_es_general(inten, exc, emis, unmix="magic")


def test_transcription_matches_chisurf():
    es = live.load("core.fluorescence.burst.es", "corrected_es", "corrected_es_matrix", "corrected_es_general")
    if es is None:
        pytest.skip("chisurf's es.py is gone; the transcription stands on its own")
    dd, da, aa = _bursts(7)
    for factors in FACTORS:
        a, b = es.corrected_es(dd, da, aa, **factors), ref.corrected_es(dd, da, aa, **factors)
        for key in ("E", "S", "fc"):
            # chisurf ran the three-cube step in C++ (FMA contraction): last-bit only
            np.testing.assert_allclose(a[key], b[key], rtol=1e-14, atol=1e-14)
    inten, gamma, alpha, delta, bg = _matrix_case(1)
    a = es.corrected_es_matrix(inten, gamma, alpha, delta, bg)
    b = ref.corrected_es_matrix(inten, gamma, alpha, delta, bg)
    for key in a:
        np.testing.assert_allclose(a[key]["E"], b[key]["E"], rtol=1e-14, atol=1e-14)
    inten, exc, emis, bg = _general_case(1)
    a = es.corrected_es_general(inten, exc, emis, background=bg)
    b = ref.corrected_es_general(inten, exc, emis, background=bg)
    for key in a:
        np.testing.assert_allclose(a[key]["E"], b[key]["E"], rtol=1e-14, atol=1e-14)
