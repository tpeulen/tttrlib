# SPDX-License-Identifier: BSD-3-Clause
"""A fit23 result must not depend on the fit run before it on the same thread.

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
