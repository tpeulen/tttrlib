"""General N-pattern fit: non-negative amplitudes of arbitrary fixed patterns.

``decay_pattern_fit`` is the first consumer of ``DecayFitProblem::patterns``:
given N fixed reference shapes and data, find non-negative amplitudes. Two
modes share one design matrix (see DecayPatternFit.h): plain NNLS (``kNone``)
and L2-regularised NNLS (``kTikhonov``). Every check here is either an
independent reference (``scipy.optimize.nnls``, on the augmented system for
the regularised case) or a property the algorithm must have by construction
(non-negativity, monotonic shrinkage with regularisation strength, exact
recovery of a noiseless mixture).
"""
import unittest

import numpy as np
import scipy.optimize
import tttrlib


def _synth_patterns(n_bins=64, taus=(1.0, 2.0, 3.0)):
    x = np.linspace(0.0, 5.0, n_bins)
    return [np.exp(-x / tau) for tau in taus]


def _fit(data, patterns, mode, lam=0.0):
    return tttrlib.decay_pattern_fit(list(data), [list(p) for p in patterns], mode, lam, 500, 1e-10)


class TestPatternFitNnls(unittest.TestCase):
    """kNone: plain NNLS, checked against scipy.optimize.nnls."""

    def test_matches_scipy_on_a_clean_mixture(self):
        patterns = _synth_patterns()
        data = sum(a * p for a, p in zip((2.0, 0.5, 3.0), patterns))
        ref_amps, _ = scipy.optimize.nnls(np.column_stack(patterns), data)
        r = _fit(data, patterns, tttrlib.PatternFitMode_kNone)
        np.testing.assert_allclose(r.amplitudes, ref_amps, atol=1e-6)

    def test_matches_scipy_with_noise_and_more_patterns(self):
        rng = np.random.default_rng(7)
        patterns = _synth_patterns(n_bins=128, taus=np.linspace(1.0, 4.0, 6))
        true_amps = rng.uniform(0.2, 5.0, len(patterns))
        data = sum(a * p for a, p in zip(true_amps, patterns))
        data = data + 0.02 * rng.standard_normal(len(data))
        ref_amps, _ = scipy.optimize.nnls(np.column_stack(patterns), data)
        r = _fit(data, patterns, tttrlib.PatternFitMode_kNone)
        np.testing.assert_allclose(r.amplitudes, ref_amps, atol=1e-4)

    def test_recovers_a_noiseless_mixture_exactly(self):
        patterns = _synth_patterns()
        true_amps = np.array([1.5, 0.0, 4.2])  # one pattern genuinely absent
        data = sum(a * p for a, p in zip(true_amps, patterns))
        r = _fit(data, patterns, tttrlib.PatternFitMode_kNone)
        np.testing.assert_allclose(r.amplitudes, true_amps, atol=1e-6)
        self.assertLess(r.chisq, 1e-12)

    def test_amplitudes_are_never_negative(self):
        rng = np.random.default_rng(3)
        patterns = _synth_patterns(taus=(1.0, 1.05, 1.1))  # near-collinear
        data = rng.uniform(0.0, 1.0, len(patterns[0]))
        r = _fit(data, patterns, tttrlib.PatternFitMode_kNone)
        self.assertTrue(all(v >= 0.0 for v in r.amplitudes))


class TestPatternFitTikhonov(unittest.TestCase):
    """kTikhonov: L2-regularised, non-negative; shrinks toward zero."""

    def test_zero_lambda_matches_plain_nnls(self):
        patterns = _synth_patterns()
        data = sum(a * p for a, p in zip((2.0, 0.5, 3.0), patterns))
        r_none = _fit(data, patterns, tttrlib.PatternFitMode_kNone)
        r_tik = _fit(data, patterns, tttrlib.PatternFitMode_kTikhonov, 0.0)
        np.testing.assert_allclose(r_tik.amplitudes, r_none.amplitudes, atol=1e-6)

    def test_matches_scipy_nnls_on_the_augmented_system(self):
        """||Ax-b||^2 + lambda ||x||^2 over x >= 0, solved independently."""
        rng = np.random.default_rng(11)
        patterns = _synth_patterns(n_bins=96, taus=(0.8, 1.0, 1.3, 2.0, 3.5))  # collinear enough to clamp
        data = sum(a * p for a, p in zip((1.0, 0.0, 2.0, 0.3, 1.5), patterns))
        data = data + 0.05 * rng.standard_normal(len(data))
        A = np.column_stack(patterns)
        for lam in (1e-3, 0.1, 1.0, 10.0):
            A_aug = np.vstack([A, np.sqrt(lam) * np.eye(A.shape[1])])
            b_aug = np.concatenate([data, np.zeros(A.shape[1])])
            ref, _ = scipy.optimize.nnls(A_aug, b_aug)
            r = _fit(data, patterns, tttrlib.PatternFitMode_kTikhonov, lam)
            np.testing.assert_allclose(r.amplitudes, ref, atol=1e-8, err_msg=f"lambda={lam}")
            x = np.asarray(r.amplitudes)
            self.assertAlmostEqual(r.chisq, float(np.sum((A @ x - data) ** 2)), delta=1e-9 * max(1.0, r.chisq))

    def test_amplitude_norm_shrinks_monotonically_with_lambda(self):
        patterns = _synth_patterns()
        data = sum(a * p for a, p in zip((2.0, 0.5, 3.0), patterns))
        norms = []
        for lam in (0.0, 1.0, 10.0, 100.0):
            r = _fit(data, patterns, tttrlib.PatternFitMode_kTikhonov, lam)
            self.assertTrue(all(v >= 0.0 for v in r.amplitudes))
            norms.append(np.linalg.norm(r.amplitudes))
        self.assertEqual(norms, sorted(norms, reverse=True))

    def test_a_negative_weight_is_refused(self):
        patterns = _synth_patterns()
        with self.assertRaises(ValueError):
            _fit(sum(patterns), patterns, tttrlib.PatternFitMode_kTikhonov, -1.0)


if __name__ == '__main__':
    unittest.main()
