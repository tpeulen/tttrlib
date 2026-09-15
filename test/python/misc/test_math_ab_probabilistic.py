"""A/B of the probabilistic kernels in `modules/math` against independent
references: `kalman_filter` and the `hmm_*` log-domain lattice.

Every kernel here already has a known-answer or fixture test of its own
(`test_kalman.py`, `test_hmm_lattice.py`).
What those cannot say is whether the kernel agrees with an implementation
nobody here wrote. This file says it, two ways per kernel where two exist:

* **A library that is not tttrlib and not ChiSurf.** hmmlearn for the lattice,
  filterpy for the Kalman filter.
  hmmlearn and filterpy are not test dependencies, so their answers are
  recorded once by ``gen_math_ab_probabilistic_reference.py`` (inputs stored
  with the outputs) into ``math_ab_probabilistic_reference.npz``.
* **A textbook implementation written here from the equations**, in NumPy,
  short enough to read in one sitting -- a second independent arrangement
  that catches a shared convention error between the kernel and the library.
**ChiSurf is not a reference**, and no longer appears here. It is a moving
target that this library is also the upstream of, so "the two agree" says only
that two things which change together still agree -- and a port's contract
("bit-exact with ChiSurf") is a statement about a snapshot, not about the
mathematics. Where a kernel began as a port, the port is a fact about its
history; what validates it is filterpy, hmmlearn or the equations.
"""

import importlib.util
import os
import sys
import unittest

import numpy as np

import tttrlib

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "data", "reference", "math_ab_probabilistic_reference.npz")

CHISURF_KALMAN = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "..", "..", "..", "..", "chisurf", "chisurf",
                              "core", "fluorescence", "burst", "kalman.py")


def _fixture():
    if not os.path.exists(FIXTURE):
        raise unittest.SkipTest("reference fixture not present")
    return np.load(FIXTURE)


def _hmm_case_names(z):
    return sorted({k.split("/")[1] for k in z.files if k.startswith("hmm/")})


def _kalman_case_names(z):
    return sorted({k.split("/")[1] for k in z.files if k.startswith("kalman/")})


# ---------------------------------------------------------------------------
# Kalman filter
# ---------------------------------------------------------------------------

def textbook_kalman(y, x0, P0, Q, dt, r_scale):
    """The filter as the header states it: A = H = I, P_pred = P + Q,
    R = diag(r_scale * max(x, 1e-12) / dt), K = P_pred S^-1, x += K v,
    P = (I - K) P_pred, D = sqrt(v^T S^-1 v). numpy.linalg.inv throughout,
    no closed forms -- a different arrangement of the same arithmetic."""
    T, dim = y.shape
    x = np.array(x0, dtype=float)
    P = np.array(P0, dtype=float)
    I = np.eye(dim)
    xs, Ps, Ds = np.empty((T, dim)), np.empty((T, dim, dim)), np.empty(T)
    for t in range(T):
        P_pred = P + Q
        R = np.diag(r_scale * np.maximum(x, 1e-12) / dt)
        v = y[t] - x
        S = P_pred + R
        S_inv = np.linalg.inv(S)
        K = P_pred @ S_inv
        x = x + K @ v
        P = (I - K) @ P_pred
        xs[t], Ps[t] = x, P
        q = v @ S_inv @ v
        Ds[t] = np.sqrt(q) if q > 0 else 0.0
    return xs, Ps, Ds


def _rel(a, b):
    return float(np.max(np.abs(a - b) / np.maximum(np.abs(b), 1e-300)))


class TestKalmanAgainstTheTextbook(unittest.TestCase):
    """Random Poisson traces in one, two and three channels against the
    NumPy statement of the recursion above. Only the arithmetic differs
    (closed-form vs LAPACK inverse, fused vs plain dot products), so
    agreement at ~1e-12 relative is what a correct port looks like; a wrong
    convention (R from the wrong state, K from P instead of P_pred, the
    Mahalanobis on the wrong innovation) is a 1e-2 event."""

    def _trace(self, rng, dim, T):
        rates = rng.uniform(2e3, 1e5, size=dim)
        jump = rng.uniform(0.3, 3.0, size=dim)
        true = np.where(np.arange(T)[:, None] < T // 2, rates, rates * jump)
        dt = 1e-3
        y = rng.poisson(true * dt).astype(np.float64) / dt
        return y, rates.copy(), np.eye(dim) * 1e6, np.eye(dim) * 100.0, dt

    def test_one_channel_is_deterministic_and_dims_beyond_four_work(self):
        """Regression: the general `K = P_pred S^-1` branch was written for
        dim == 4 (strides of 4, four terms), so dim == 1 read past its
        1-element vectors -- undefined behaviour that usually met zeroed heap
        slack and now and then did not, an intermittent one-channel failure of
        the test below -- and dim >= 5 was refused. Interleaving shapes is what
        made the stale heap show; 20 interleaved repetitions must be identical
        and every dim must match the textbook."""
        rng = np.random.default_rng(23)
        cases = []
        for dim in (1, 2, 3, 4, 5, 6):
            y, x0, P0, Q, dt = self._trace(rng, dim, 120)
            cases.append((dim, y, x0, P0, Q, dt, 1.0))
        first = {}
        for _ in range(20):
            for dim, y, x0, P0, Q, dt, r_scale in cases:
                got = [np.array(g) for g in tttrlib.kalman_filter(y, x0, P0, Q, dt, r_scale)]
                if dim in first:
                    for a, b in zip(got, first[dim]):
                        np.testing.assert_array_equal(a, b)
                else:
                    first[dim] = got
                    ref = textbook_kalman(y, x0, P0, Q, dt, r_scale)
                    for g, r, name in zip(got, ref, ("x", "P", "D")):
                        self.assertLess(_rel(g, r), 1e-11, f"dim={dim} {name}")

    def test_one_two_and_three_channels(self):
        rng = np.random.default_rng(11)
        for dim in (1, 2, 3):
            for _ in range(5):
                y, x0, P0, Q, dt = self._trace(rng, dim, int(rng.integers(50, 500)))
                r_scale = float(rng.uniform(0.5, 2.0))
                got = tttrlib.kalman_filter(y, x0, P0, Q, dt, r_scale)
                ref = textbook_kalman(y, x0, P0, Q, dt, r_scale)
                for g, r, name in zip(got, ref, ("x", "P", "D")):
                    self.assertLess(_rel(g, r), 1e-11, f"dim={dim} {name}")


class TestKalmanAgainstFilterpy(unittest.TestCase):
    """The recorded filterpy answers. filterpy updates the covariance in the
    Joseph form (I-KH) P (I-KH)^T + K R K^T, which is algebraically the same
    matrix and numerically a different one, so this is not a transcription
    check -- see the generator's docstring."""

    def test_recorded_cases(self):
        z = _fixture()
        names = _kalman_case_names(z)
        self.assertGreaterEqual(len(names), 3)
        for n in names:
            g = lambda s: z[f"kalman/{n}/{s}"]
            got = tttrlib.kalman_filter(g("y"), g("x0"), g("P0"), g("Q"),
                                        float(g("dt")), float(g("r_scale")))
            for got_i, key in zip(got, ("x_filt", "P_filt", "D")):
                self.assertLess(_rel(got_i, g(key)), 1e-11, f"{n} {key}")


# ChiSurf's `_kalman_filter_loop` used to be checked here as a third
# reference. It is not one: it is a moving target that this library is the
# upstream of, so agreement says only that two things which change together
# still agree. The claim it made -- this recursion, bit for bit -- is the claim
# `TestKalmanAgainstFilterpy` and `TestKalmanAgainstTheTextbook` make against
# code that has never seen ours.


# ---------------------------------------------------------------------------
# HMM lattice
# ---------------------------------------------------------------------------

def _lse(a, axis=None):
    from scipy.special import logsumexp
    with np.errstate(divide="ignore", invalid="ignore"):
        return logsumexp(a, axis=axis)


def textbook_forward_backward(log_start, log_trans, log_frame):
    """Forward-backward straight from the recursions, one logsumexp per
    cell, plus posteriors and the summed xi. Returns
    (log_prob, fwd, bwd, posteriors, xi_sum)."""
    T, K = log_frame.shape
    fwd = np.empty((T, K))
    fwd[0] = log_start + log_frame[0]
    for t in range(1, T):
        fwd[t] = _lse(fwd[t - 1][:, None] + log_trans, axis=0) + log_frame[t]
    log_prob = _lse(fwd[-1])
    bwd = np.zeros((T, K))
    for t in range(T - 2, -1, -1):
        bwd[t] = _lse(log_trans + (log_frame[t + 1] + bwd[t + 1])[None, :], axis=1)
    post = np.exp(fwd + bwd - log_prob)
    xi = np.zeros((K, K))
    for t in range(T - 1):
        xi += np.exp(fwd[t][:, None] + log_trans + (log_frame[t + 1] + bwd[t + 1])[None, :]
                     - log_prob)
    return log_prob, fwd, bwd, post, xi


def textbook_viterbi(log_start, log_trans, log_frame):
    T, K = log_frame.shape
    delta = log_start + log_frame[0]
    back = np.zeros((T, K), dtype=np.int64)
    for t in range(1, T):
        cand = delta[:, None] + log_trans           # (from, to)
        back[t] = np.argmax(cand, axis=0)           # first max = lowest index
        delta = cand[back[t], np.arange(K)] + log_frame[t]
    path = np.empty(T, dtype=np.int64)
    path[-1] = int(np.argmax(delta))
    for t in range(T - 1, 0, -1):
        path[t - 1] = back[t, path[t]]
    return float(np.max(delta)), path


def _run_lattice(log_start, log_trans, log_frame):
    fwd = np.empty_like(log_frame)
    lp = tttrlib.hmm_forward_log(log_start, log_trans, log_frame, fwd)
    bwd = np.empty_like(log_frame)
    tttrlib.hmm_backward_log(log_trans, log_frame, bwd)
    post = np.empty_like(log_frame)
    xi = np.zeros((log_frame.shape[1],) * 2)
    tttrlib.hmm_backward_posteriors_xi(log_trans, log_frame, fwd, lp, post, xi)
    states = np.empty(log_frame.shape[0], dtype=np.int64)
    vs = tttrlib.hmm_viterbi_log(log_start, log_trans, log_frame, states)
    return lp, fwd, bwd, post, xi, vs, states


class TestHmmLatticeAgainstHmmlearn(unittest.TestCase):
    """Recorded from hmmlearn's `_hmmc` (forward_log, backward_log,
    compute_log_xi_sum, viterbi) on random 2/3/5-state models, a T=1
    sequence, a forbidden transition and a frame one state cannot explain.
    The two lattices are the same recursion in the same order, so forward and
    backward are expected identical to the ulp; posteriors and xi are one
    fused sweep here against three passes there, so those carry
    accumulation-order noise (~1e-14 / ~1e-13 observed) and are compared at
    1e-12 / 1e-11."""

    def test_recorded_cases(self):
        z = _fixture()
        names = _hmm_case_names(z)
        self.assertGreaterEqual(len(names), 5)
        for n in names:
            g = lambda s: z[f"hmm/{n}/{s}"]
            frame = np.ascontiguousarray(g("log_frameprob"))
            lp, fwd, bwd, post, xi, vs, states = _run_lattice(
                g("log_startprob"), g("log_transmat"), frame)
            self.assertAlmostEqual(lp, float(g("log_prob")), places=12, msg=n)
            np.testing.assert_allclose(fwd, g("fwd"), rtol=0, atol=1e-12, err_msg=n)
            np.testing.assert_allclose(bwd, g("bwd"), rtol=0, atol=1e-12, err_msg=n)
            np.testing.assert_allclose(post, g("posteriors"), rtol=0, atol=1e-12, err_msg=n)
            np.testing.assert_allclose(xi, g("xi_sum"), rtol=1e-11, atol=1e-11, err_msg=n)
            self.assertAlmostEqual(vs, float(g("viterbi_score")), places=12, msg=n)
            np.testing.assert_array_equal(states, g("viterbi_path"), err_msg=n)

    def test_estep_over_the_concatenated_cases_is_the_sum(self):
        """`hmm_estep_log` on the recorded sequences laid end to end must give
        hmmlearn's per-sequence log-likelihoods, and a xi_sum that is the sum
        of hmmlearn's per-sequence xi -- which is exactly how hmmlearn's own
        E-step accumulates over `lengths`."""
        z = _fixture()
        names = [n for n in _hmm_case_names(z)
                 if z[f"hmm/{n}/log_frameprob"].shape[1] == 3]
        self.assertGreaterEqual(len(names), 2)
        start = z[f"hmm/{names[0]}/log_startprob"]
        trans = z[f"hmm/{names[0]}/log_transmat"]
        # one model, several sequences: re-score every 3-state sequence under
        # the first case's parameters with hmmlearn's answer recomputed here
        # by the textbook code (hmmlearn is not importable in-process).
        frames = [np.ascontiguousarray(z[f"hmm/{n}/log_frameprob"]) for n in names]
        frame = np.ascontiguousarray(np.vstack(frames))
        lengths = np.asarray([f.shape[0] for f in frames], dtype=np.int64)
        fwd = np.empty_like(frame)
        post = np.empty_like(frame)
        xi = np.zeros((3, 3))
        per_seq = np.empty(len(frames))
        total = tttrlib.hmm_estep_log(start, trans, frame, lengths, fwd, post, xi, per_seq)
        exp_total, exp_xi = 0.0, np.zeros((3, 3))
        for i, f in enumerate(frames):
            lp, _, _, _, xi_i = textbook_forward_backward(start, trans, f)
            exp_total += lp
            exp_xi += xi_i
            self.assertAlmostEqual(per_seq[i], lp, places=10)
        self.assertAlmostEqual(total, exp_total, places=9)
        np.testing.assert_allclose(xi, exp_xi, rtol=1e-10, atol=1e-11)


class TestHmmLatticeAgainstTheTextbook(unittest.TestCase):
    """The recursions written out in NumPy with scipy's logsumexp, on random
    models up to eight states -- an independent arrangement that would
    disagree with the fused sweep if the fusion dropped a term."""

    def test_random_models(self):
        rng = np.random.default_rng(5)
        for K, T in ((2, 40), (3, 90), (5, 120), (8, 60)):
            start = np.log(rng.dirichlet(np.ones(K)))
            trans = np.log(rng.dirichlet(np.ones(K), size=K))
            frame = np.ascontiguousarray(np.log(rng.random((T, K)) * 0.95 + 0.05))
            lp, fwd, bwd, post, xi, vs, states = _run_lattice(start, trans, frame)
            rlp, rfwd, rbwd, rpost, rxi = textbook_forward_backward(start, trans, frame)
            self.assertAlmostEqual(lp, rlp, places=10)
            np.testing.assert_allclose(fwd, rfwd, rtol=1e-12, atol=1e-10)
            np.testing.assert_allclose(bwd, rbwd, rtol=1e-12, atol=1e-10)
            np.testing.assert_allclose(post, rpost, rtol=1e-10, atol=1e-12)
            np.testing.assert_allclose(xi, rxi, rtol=1e-10, atol=1e-11)
            rvs, rpath = textbook_viterbi(start, trans, frame)
            self.assertAlmostEqual(vs, rvs, places=10)
            np.testing.assert_array_equal(states, rpath)

    def test_logsumexp_matches_scipy_including_minus_infinity(self):
        from scipy.special import logsumexp
        rng = np.random.default_rng(2)
        for v in (rng.normal(size=7) * 50, np.array([-np.inf, -3.0, -1000.0]),
                  np.full(4, -np.inf), np.array([700.0, 700.0])):
            got = tttrlib.hmm_logsumexp(np.ascontiguousarray(v))
            with np.errstate(divide="ignore"):
                ref = float(logsumexp(v))
            if np.isinf(ref):
                self.assertEqual(got, ref)
            else:
                self.assertAlmostEqual(got, ref, places=12)


if __name__ == "__main__":
    unittest.main()
