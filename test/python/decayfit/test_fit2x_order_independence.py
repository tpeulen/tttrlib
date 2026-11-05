# SPDX-License-Identifier: BSD-3-Clause
"""A Fit2x result must not depend on the fit run before it, the row order or the thread count.

DecayFit23 keeps its corrections (gamma among them) in thread_local state, and
``fit``/``evaluate`` used to take ``Bexpected = gamma * B`` -- the soft-BIFL
term's expected background -- before setting gamma from the new start values,
so at that point it held the *previous* fit's gamma. The objective's own
evaluations recompute it, which is why no result differed in the cases below
when this was written; the fix sets gamma first, and this test keeps every
combination of soft BIFL, P+2S and parameter links order-independent, for a
single fit, a score and a batch.
"""
from __future__ import annotations

import numpy as np
import pytest

tttrlib = pytest.importorskip("tttrlib")

N = 128
DT = 0.25
T = np.arange(N) * DT
_IRF_HALF = np.exp(-0.5 * ((T - 2.0) / 0.15) ** 2)
IRF = np.concatenate([_IRF_HALF, _IRF_HALF])
BG = np.full(2 * N, 1.0 / (2 * N))


def _decay(tau, n_photons, seed, background=0.0):
    rng = np.random.default_rng(seed)
    shape = np.convolve(_IRF_HALF, np.exp(-T / tau))[:N]
    shape /= shape.sum()
    d = np.concatenate([rng.poisson(shape * n_photons * 0.6), rng.poisson(shape * n_photons * 0.4)])
    return (d + rng.poisson(background, 2 * N)).astype(float)


A = _decay(1.0, 200, 5, background=2.0)  # background-heavy: a large gamma
B = _decay(4.0, 60, 3)                    # a dim burst: a small gamma


def _problem(data=None):
    p = tttrlib.DecayFitProblem(2, N, DT)
    p.irf = tttrlib.VectorDouble(IRF.tolist())
    p.background = tttrlib.VectorDouble(BG.tolist())
    if data is not None:
        p.data = tttrlib.VectorDouble(np.asarray(data).tolist())
    return p


@pytest.mark.parametrize("soft_bifl", [False, True])
@pytest.mark.parametrize("p2s", [False, True])
@pytest.mark.parametrize("link", [[0, 0, -1, 0], [0, -1, -1, 0], [0, -1, -1, -1]])
def test_a_fit_does_not_depend_on_the_fit_before_it(soft_bifl, p2s, link):
    setup = list(tttrlib.setup_vector(
        "fit23", dt=DT, period=32.0, g_factor=1.0, l1=0.0, l2=0.0,
        soft_bifl_scatter_flag=soft_bifl, objective="p2s_mle" if p2s else "poisson_mle"))
    fit = tttrlib.DecayFit2("fit23", setup, IRF.tolist())
    cons = tttrlib.DecayFitConstraints(tttrlib.VectorInt32(link))
    xa, xb = [3.0, 0.7, 0.38, 1.0], [3.0, 0.1, 0.38, 1.0]

    def one(data, x0):
        out = fit.fit(x0, cons, _problem(data))
        return np.asarray(list(out.parameters) + [out.objective] + list(out.results))

    alone = one(B, xb)
    one(A, xa)
    after = one(B, xb)
    np.testing.assert_array_equal(after, alone)

    score_alone = fit.evaluate(xb, _problem(B))
    one(A, xa)
    np.testing.assert_array_equal(fit.evaluate(xb, _problem(B)), score_alone)

    batch_alone = fit.fit_many(_problem(), B.tolist(), 1, 2 * N, xb, cons)
    batch_after = fit.fit_many(_problem(), np.concatenate([A, B]).tolist(), 2, 2 * N, xb, cons)
    np.testing.assert_array_equal(np.asarray(batch_after.parameters)[4:], np.asarray(batch_alone.parameters))
    np.testing.assert_array_equal(np.asarray(batch_after.objective)[1:], np.asarray(batch_alone.objective))


# ---- every Fit2x model, every row order, every thread count -----------------------------

_MODELS = {
    # name: (start values, link codes); fit25's taus are its fixed lifetime grid
    "fit23": ([3.0, 0.1, 0.38, 1.0], [0, 0, -1, 0]),
    "fit24": ([2.0, 0.1, 0.5, 0.3, 0.0], [0, 0, 0, 0, -1]),
    "fit25": ([0.5, 1.5, 3.0, 4.5, 0.1, 0.38], [-1, -1, -1, -1, 0, -1]),
    "fit26": ([0.3], [0]),
}


def _rows(n_rows=40, seed=11):
    rng = np.random.default_rng(seed)
    out = []
    for k in range(n_rows):
        out.append(_decay(float(rng.uniform(0.5, 5.0)), int(rng.integers(30, 400)), seed + k,
                          background=float(rng.uniform(0.0, 2.0))))
    return np.asarray(out)


def _batch(fit, rows, x0, link):
    out = fit.fit_many(_problem(), rows.ravel().tolist(), rows.shape[0], 2 * N, list(x0),
                       tttrlib.DecayFitConstraints(tttrlib.VectorInt32(link)))
    n_par = len(out.parameters) // rows.shape[0]
    n_res = len(out.results) // rows.shape[0]
    return (np.asarray(out.parameters).reshape(rows.shape[0], n_par),
            np.asarray(out.objective),
            np.asarray(out.results).reshape(rows.shape[0], max(n_res, 1)) if n_res else None)


@pytest.mark.parametrize("model", sorted(_MODELS))
@pytest.mark.parametrize("soft_bifl", [False, True])
@pytest.mark.parametrize("p2s", [False, True])
def test_batch_rows_do_not_depend_on_order_or_threads(monkeypatch, model, soft_bifl, p2s):
    x0, link = _MODELS[model]
    setup = list(tttrlib.setup_vector(
        model, dt=DT, period=32.0, g_factor=1.0, l1=0.0, l2=0.0,
        soft_bifl_scatter_flag=soft_bifl, objective="p2s_mle" if p2s else "poisson_mle"))
    fit = tttrlib.DecayFit2(model, setup, IRF.tolist())
    n_par = fit.n_parameters(_problem())
    x0 = (list(x0) + [0.0] * n_par)[:n_par]
    link = (list(link) + [-1] * n_par)[:n_par]
    rows = _rows(1100)  # above fit_batch's 1024-row threading threshold
    order = np.random.default_rng(3).permutation(rows.shape[0])
    monkeypatch.setenv("TTTRLIB_NUM_THREADS", "1")
    ref = _batch(fit, rows, x0, link)
    for threads in ("4", "8"):
        monkeypatch.setenv("TTTRLIB_NUM_THREADS", threads)
        got = _batch(fit, rows, x0, link)
        np.testing.assert_array_equal(got[0], ref[0], err_msg=f"{model} parameters, {threads} threads")
        np.testing.assert_array_equal(got[1], ref[1], err_msg=f"{model} 2I*, {threads} threads")
    shuffled = _batch(fit, rows[order], x0, link)
    np.testing.assert_array_equal(shuffled[0], ref[0][order], err_msg=f"{model} parameters, shuffled")
    np.testing.assert_array_equal(shuffled[1], ref[1][order], err_msg=f"{model} 2I*, shuffled")
    # one row alone, after the whole batch ran on this thread
    one = _batch(fit, rows[order[:1]], x0, link)
    np.testing.assert_array_equal(one[0][0], ref[0][order[0]], err_msg=f"{model} a row alone")
    # the problem the caller handed in is not changed by a fit (fit26 normalised it in place)
    p = _problem(rows[0])
    irf_before = np.asarray(p.irf).copy()
    fit.fit(x0, tttrlib.DecayFitConstraints(tttrlib.VectorInt32(link)), p)
    np.testing.assert_array_equal(np.asarray(p.irf), irf_before, err_msg=f"{model} changed the IRF")
