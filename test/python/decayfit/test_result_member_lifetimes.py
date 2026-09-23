"""Result structs own their array members -- reading them off a temporary is safe.

SWIG's default for a ``std::vector<double>`` *member* is to hand Python a
pointer into the C++ object rather than a copy. The moment the owning result
proxy is collected, that vector dangles, so

    decay_pattern_fit(...).amplitudes   # temporary freed before the member is read

read freed memory while

    r = decay_pattern_fit(...); r.amplitudes   # owner still alive

works fine. That asymmetry is the whole danger: the binding *appears* correct
for as long as every caller happens to bind the result to a name, and the first
one-liner gets either an ``OverflowError`` from a nonsense length, a NumPy
conversion failure, or -- on a different allocation -- plausible wrong numbers
with no error at all.

``DecayFit.i`` diagnosed and fixed this for ``DecayFitOutcome`` and friends with
``%naturalvar``; ``DecayPatternFit.i`` had the same bug and was missed. This pins
it so the next struct that grows a vector member is caught here instead of in a
caller.
"""
import unittest

import numpy as np
import tttrlib


class TestPatternFitResultLifetime(unittest.TestCase):
    """``PatternFitResult::amplitudes`` survives its owner being a temporary."""

    @staticmethod
    def _patterns(n_bins=64):
        x = np.linspace(0.0, 5.0, n_bins)
        return [np.exp(-x / tau) for tau in (1.0, 2.0, 3.0)]

    def _call(self):
        patterns = self._patterns()
        data = 0.5 * patterns[0] + 0.3 * patterns[2]
        return tttrlib.decay_pattern_fit(
            data.tolist(), [p.tolist() for p in patterns], tttrlib.PatternFitMode_kNone
        )

    def test_bound_and_temporary_agree(self):
        expected = np.array(self._call().amplitudes, dtype=float, copy=True)
        got = np.array(self._call().amplitudes, dtype=float)
        self.assertEqual(got.shape, (3,))
        np.testing.assert_allclose(got, expected, rtol=1e-12, atol=0.0)

    def test_temporary_member_has_the_right_length(self):
        self.assertEqual(len(self._call().amplitudes), 3)


if __name__ == "__main__":
    unittest.main()
