import unittest
import numpy as np
import tttrlib


class TestSpectralCrosstalk(unittest.TestCase):

    def test_three_cube_basic(self):
        r = list(tttrlib.correct_three_cube(
            100.0, 50.0, 80.0, gamma=0.8, alpha=0.05, delta=0.1))
        self.assertEqual(len(r), 3)  # E, S, Fc
        self.assertTrue(0 <= r[0] <= 1)  # E in [0, 1]
        self.assertTrue(0 <= r[1] <= 1)  # S in [0, 1]

    def test_three_cube_no_crosstalk(self):
        # alpha=0, delta=0, gamma=1: pure proximity ratio
        r = list(tttrlib.correct_three_cube(
            100.0, 50.0, 80.0, gamma=1.0, alpha=0.0, delta=0.0))
        E_expected = 50.0 / (50.0 + 100.0)
        self.assertAlmostEqual(r[0], E_expected, places=6)

    def test_batch(self):
        n = 100
        i_dd = list(np.random.uniform(50, 200, n))
        i_da = list(np.random.uniform(10, 100, n))
        i_aa = list(np.random.uniform(50, 200, n))
        r = list(tttrlib.correct_three_cube_batch(
            i_dd, i_da, i_aa, gamma=0.8, alpha=0.05, delta=0.1))
        self.assertEqual(len(r), 3 * n)

    def test_invert_mixing_ridge(self):
        # 2x2 identity mixing: sources == measured
        M = [1.0, 0.0, 0.0, 1.0]
        measured = [3.0, 7.0]
        x = list(tttrlib.invert_mixing_ridge(M, measured, 2, 2, 0.0))
        self.assertAlmostEqual(x[0], 3.0, places=4)
        self.assertAlmostEqual(x[1], 7.0, places=4)


class TestBackgroundEstimation(unittest.TestCase):

    def test_pure_poisson(self):
        np.random.seed(42)
        rate_hz = 5000
        ipt_ms = np.random.exponential(1000.0 / rate_hz, 100000)
        bg = tttrlib.estimate_background_rate(ipt_ms.tolist(), 0.1, 1.0)
        # MLE on inter-photon times in ms -> rate in 1/ms = kHz
        self.assertAlmostEqual(bg, 5.0, delta=0.5)  # ~5 kHz

    def test_empty_input(self):
        bg = tttrlib.estimate_background_rate([], 0.1, 0.5)
        self.assertEqual(bg, 0.0)


if __name__ == '__main__':
    unittest.main()
