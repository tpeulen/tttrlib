"""
Comprehensive test suite to verify AVX convolution implementations
against default implementations for correctness.

This test suite checks:
1. fconv_simd vs fconv
2. fconv_per_simd vs fconv_per

With various test cases including:
- Different numbers of exponentials (1, 2, 4, 5, 8, 16)
- Different lifetime values
- Different IRF shapes
- Edge cases (zero lifetimes, very short/long lifetimes)
"""

import os
import subprocess
import sys
import unittest
import numpy as np
import scipy.stats
import pytest
import tttrlib
from misc.compute_irf import model_irf


@unittest.skipUnless(tttrlib.get_avx_enabled(), "AVX support not available; skipping AVX convolution tests")
class TestAVXConvolutionCorrectness(unittest.TestCase):
    """Test AVX convolution implementations against default implementations.
    
    Note: This test class is marked as xfail when AVX is not available.
    This means failures are expected and recorded but do not fail the test suite.
    """

    def setUp(self):
        """Set up common test parameters."""
        self.tolerance = 1e-10  # Very strict tolerance for numerical accuracy
        
    def generate_irf(self, n_channels=64, period=12.0, irf_position=2.0, irf_width=0.25):
        """Generate a model IRF for testing."""
        irf, time_axis = model_irf(
            n_channels=n_channels,
            period=period,
            irf_position_p=irf_position,
            irf_position_s=irf_position,
            irf_width=irf_width
        )
        return irf, time_axis

    def test_fconv_simd_single_exponential(self):
        """Test fconv_simd with a single exponential."""
        print("\n=== Testing fconv_simd: Single Exponential ===")
        
        irf, time_axis = self.generate_irf(n_channels=64)
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([1.0, 4.1])
        
        # Default implementation
        model_default = np.zeros_like(irf)
        tttrlib.fconv(
            fit=model_default,
            irf=irf,
            x=lifetime_spectrum,
            dt=dt
        )
        
        # AVX implementation
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_simd(
            fit=model_avx,
            irf=irf,
            x=lifetime_spectrum,
            dt=dt
        )
        
        # Compare
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_simd_two_exponentials(self):
        """Test fconv_simd with two exponentials."""
        print("\n=== Testing fconv_simd: Two Exponentials ===")
        
        irf, time_axis = self.generate_irf(n_channels=64)
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.6, 2.5, 0.4, 5.8])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv(fit=model_default, irf=irf, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_simd_four_exponentials(self):
        """Test fconv_simd with four exponentials (exactly one AVX register)."""
        print("\n=== Testing fconv_simd: Four Exponentials (1 AVX register) ===")
        
        irf, time_axis = self.generate_irf(n_channels=64)
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.3, 1.5, 0.25, 3.2, 0.25, 5.0, 0.2, 7.5])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv(fit=model_default, irf=irf, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_simd_five_exponentials(self):
        """Test fconv_simd with five exponentials (requires padding)."""
        print("\n=== Testing fconv_simd: Five Exponentials (requires padding) ===")
        
        irf, time_axis = self.generate_irf(n_channels=64)
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.25, 1.2, 0.2, 2.8, 0.2, 4.5, 0.2, 6.2, 0.15, 8.0])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv(fit=model_default, irf=irf, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_simd_eight_exponentials(self):
        """Test fconv_simd with eight exponentials (two AVX registers)."""
        print("\n=== Testing fconv_simd: Eight Exponentials (2 AVX registers) ===")
        
        irf, time_axis = self.generate_irf(n_channels=64)
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([
            0.15, 0.8, 0.15, 1.5, 0.15, 2.5, 0.15, 3.5,
            0.1, 4.5, 0.1, 5.5, 0.1, 6.5, 0.1, 7.5
        ])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv(fit=model_default, irf=irf, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_simd_extreme_lifetimes(self):
        """Test fconv_simd with very short and very long lifetimes."""
        print("\n=== Testing fconv_simd: Extreme Lifetimes ===")
        
        irf, time_axis = self.generate_irf(n_channels=64)
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.5, 0.1, 0.3, 15.0, 0.2, 50.0])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv(fit=model_default, irf=irf, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_simd_different_irf_shapes(self):
        """Test fconv_simd with different IRF shapes."""
        print("\n=== Testing fconv_simd: Different IRF Shapes ===")
        
        lifetime_spectrum = np.array([0.6, 3.5, 0.4, 6.0])
        
        # Test with narrow IRF
        irf_narrow, time_axis = self.generate_irf(n_channels=64, irf_width=0.1)
        dt = time_axis[1] - time_axis[0]
        
        model_default = np.zeros_like(irf_narrow)
        tttrlib.fconv(fit=model_default, irf=irf_narrow, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf_narrow)
        tttrlib.fconv_simd(fit=model_avx, irf=irf_narrow, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference (narrow IRF): {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)
        
        # Test with wide IRF
        irf_wide, time_axis = self.generate_irf(n_channels=64, irf_width=0.5)
        dt = time_axis[1] - time_axis[0]
        
        model_default = np.zeros_like(irf_wide)
        tttrlib.fconv(fit=model_default, irf=irf_wide, x=lifetime_spectrum, dt=dt)
        
        model_avx = np.zeros_like(irf_wide)
        tttrlib.fconv_simd(fit=model_avx, irf=irf_wide, x=lifetime_spectrum, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference (wide IRF): {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_single_exponential(self):
        """Test fconv_per_simd with a single exponential."""
        print("\n=== Testing fconv_per_simd: Single Exponential ===")
        
        period = 13.0
        irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([1.0, 4.1])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(
            fit=model_default,
            irf=irf,
            x=lifetime_spectrum,
            period=period,
            start=0,
            stop=-1,
            dt=dt
        )
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(
            fit=model_avx,
            irf=irf,
            x=lifetime_spectrum,
            period=period,
            start=0,
            stop=-1,
            dt=dt
        )
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_two_exponentials(self):
        """Test fconv_per_simd with two exponentials."""
        print("\n=== Testing fconv_per_simd: Two Exponentials ===")
        
        period = 13.0
        irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.6, 2.5, 0.4, 5.8])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_four_exponentials(self):
        """Test fconv_per_simd with four exponentials."""
        print("\n=== Testing fconv_per_simd: Four Exponentials ===")
        
        period = 13.0
        irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.3, 1.5, 0.25, 3.2, 0.25, 5.0, 0.2, 7.5])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_five_exponentials(self):
        """Test fconv_per_simd with five exponentials (requires padding)."""
        print("\n=== Testing fconv_per_simd: Five Exponentials ===")
        
        period = 13.0
        irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.25, 1.2, 0.2, 2.8, 0.2, 4.5, 0.2, 6.2, 0.15, 8.0])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_eight_exponentials(self):
        """Test fconv_per_simd with eight exponentials."""
        print("\n=== Testing fconv_per_simd: Eight Exponentials ===")
        
        period = 13.0
        irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([
            0.15, 0.8, 0.15, 1.5, 0.15, 2.5, 0.15, 3.5,
            0.1, 4.5, 0.1, 5.5, 0.1, 6.5, 0.1, 7.5
        ])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_different_periods(self):
        """Test fconv_per_simd with different period values."""
        print("\n=== Testing fconv_per_simd: Different Periods ===")
        
        lifetime_spectrum = np.array([0.6, 3.5, 0.4, 6.0])
        
        for period in [10.0, 15.0, 20.0, 30.0]:
            print(f"  Testing period = {period}")
            irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
            irf[irf < 0.001] = 0.0
            dt = time_axis[1] - time_axis[0]
            
            model_default = np.zeros_like(irf)
            tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
            
            model_avx = np.zeros_like(irf)
            tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
            
            max_diff = np.max(np.abs(model_default - model_avx))
            print(f"    Max difference: {max_diff:.2e}")
            np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_extreme_lifetimes(self):
        """Test fconv_per_simd with very short and very long lifetimes."""
        print("\n=== Testing fconv_per_simd: Extreme Lifetimes ===")
        
        period = 13.0
        irf, time_axis = self.generate_irf(n_channels=64, period=period, irf_width=0.15)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.5, 0.1, 0.3, 15.0, 0.2, 50.0])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)

    def test_fconv_per_simd_large_dataset(self):
        """Test fconv_per_simd with a larger dataset."""
        print("\n=== Testing fconv_per_simd: Large Dataset ===")
        
        period = 25.0
        irf, time_axis = self.generate_irf(n_channels=256, period=period, irf_width=0.2)
        irf[irf < 0.001] = 0.0
        dt = time_axis[1] - time_axis[0]
        lifetime_spectrum = np.array([0.3, 2.0, 0.3, 4.5, 0.2, 7.0, 0.2, 10.0])
        
        model_default = np.zeros_like(irf)
        tttrlib.fconv_per(fit=model_default, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        model_avx = np.zeros_like(irf)
        tttrlib.fconv_per_simd(fit=model_avx, irf=irf, x=lifetime_spectrum, period=period, start=0, stop=-1, dt=dt)
        
        max_diff = np.max(np.abs(model_default - model_avx))
        print(f"Max difference: {max_diff:.2e}")
        np.testing.assert_allclose(model_avx, model_default, rtol=self.tolerance, atol=self.tolerance)


class TestTheScalarAndSimdKernelsActuallyAgree(unittest.TestCase):
    """The comparison the rest of this file was meant to make.

    Every other test here compares ``fconv_per`` against ``fconv_per_simd``.
    Since the deprecation those are the *same function* — `fconv_per_simd`
    forwards to `fconv_per`, which picks the kernel itself — so those tests
    compare a function with itself and cannot fail. That is why a real
    divergence lived here undetected: the scalar kernel accumulated into `fit`
    while both SIMD kernels cleared it first, so `fconv_per` overwrote at
    ``numexp >= 2`` and accumulated at ``numexp == 1`` (one lifetime is below
    the SIMD threshold, so it is always scalar), on one machine, with no
    environment variable set.

    The second reason it survived: every other class in this file is
    ``@skipUnless(get_avx_enabled())``, so on AArch64 — the platform this is
    developed on — all fifteen of them skip, and a skip reads like a pass. This
    class is deliberately not gated: it drives whichever SIMD family the build
    has through the environment override, so it runs everywhere.

    Selecting the other kernel needs a separate process — the dispatch reads
    ``TTTRLIB_USE_NEON`` / ``TTTRLIB_USE_AVX`` through the feature detection —
    so that is what these do.
    """

    @staticmethod
    def run_in(env_overrides, body):
        code = (
            "import numpy as np, tttrlib\n"
            "n = 64\n"
            "irf = np.exp(-0.5 * ((np.arange(n) - 10) / 3.0) ** 2)\n"
            + body
        )
        out = subprocess.run([sys.executable, "-c", code],
                             env=dict(os.environ, **env_overrides),
                             capture_output=True, text=True)
        if out.returncode != 0:
            raise AssertionError("child failed: " + out.stderr[-2000:])
        return np.array([float(v) for v in out.stdout.split()])

    def test_the_two_kernels_give_the_same_curve(self):
        """Same input, both dispatch paths, compared across a process boundary."""
        body = (
            "x = np.array([0.6, 2.5, 0.4, 5.8])\n"
            "fit = np.zeros(n)\n"
            "tttrlib.fconv_per(fit=fit, irf=irf, x=x, period=13.0, start=0, stop=-1, dt=0.2)\n"
            "print(' '.join('%.17g' % v for v in fit))\n"
        )
        simd = self.run_in({"TTTRLIB_USE_NEON": "1", "TTTRLIB_USE_AVX": "1"}, body)
        scalar = self.run_in({"TTTRLIB_USE_NEON": "0", "TTTRLIB_USE_AVX": "0"}, body)
        np.testing.assert_allclose(simd, scalar, rtol=1e-12, atol=1e-12)

    def test_a_stop_short_of_the_point_count_reads_only_its_own_buffer(self):
        """`fconv_per` was not a pure function, and this is the shape of it.

        The precomputed `dt/2 * lamp` buffer was sized by `stop`, but the
        recursion runs to `stop1`, which is bounded by the *point count* and not
        by `stop`. Any caller passing a stop short of the point count — and
        `stop = n_points - 1` is enough — read past the end of that buffer and
        convolved with whatever the heap held there. It presented as state: with
        a freshly allocated response and a freshly zeroed output on every call,
        the answer still grew by about one species' worth per call, because the
        arrays freed by the previous call were what lay past the end.

        So the assertion is repetition, not a reference value: the same inputs
        must give the same answer, call after call, with everything else in the
        process churning the heap in between.
        """
        body = (
            "K = 33\n"
            "irf = np.exp(-0.5 * ((np.arange(n) - 10) / 0.9) ** 2)\n"
            "irf = irf / irf.sum()\n"
            "x = np.empty(2 * K)\n"
            "x[0::2] = 1.0 / K\n"
            "x[1::2] = np.geomspace(0.05, 60.0, K)\n"
            "out = []\n"
            "for _ in range(6):\n"
            "    response = np.ascontiguousarray(irf.copy())\n"
            "    fit = np.zeros(n)\n"
            "    tttrlib.fconv_per(fit, response, x, 13.0, 0, n - 1, 13.0 / n)\n"
            "    out.append(fit.max())\n"
            "print(' '.join('%.17g' % v for v in out))\n"
        )
        for label, env in (("simd", {"TTTRLIB_USE_NEON": "1", "TTTRLIB_USE_AVX": "1"}),
                           ("scalar", {"TTTRLIB_USE_NEON": "0", "TTTRLIB_USE_AVX": "0"})):
            with self.subTest(kernel=label):
                values = self.run_in(env, body)
                self.assertTrue(
                    np.all(values == values[0]),
                    "the %s kernel returned %d different answers for one input: %s"
                    % (label, len(set(values.tolist())), values))

    def test_fconv_per_and_fconv_per_cs_agree_over_the_full_range(self):
        """Pins the *value*, which repetition alone does not.

        The overrun fix made `fconv_per` stable; stable is not the same as
        right. Over the full range (`stop = n_points`, no convolution stop)
        these two independently written kernels produce the same curve bit for
        bit, which is a much stronger statement than any tolerance — and the
        reporter's own unfixed build gave this value on its *first* call, before
        the growth started, which is the other half of the confirmation.

        With `stop = n_points - 1` the last bin differs, and should: the tail
        loop is `i < stop`, so bin `stop` never receives its periodic tail.
        That is what `stop` means, and it is asserted here so it is not
        mistaken for the overrun coming back.
        """
        body = (
            "K = 33\n"
            "irf = np.exp(-0.5 * ((np.arange(n) - 10) / 0.9) ** 2)\n"
            "irf = irf / irf.sum()\n"
            "x = np.empty(2 * K)\n"
            "x[0::2] = 1.0 / K\n"
            "x[1::2] = np.geomspace(0.05, 60.0, K)\n"
            "dt = 13.0 / n\n"
            "full = np.zeros(n)\n"
            "tttrlib.fconv_per(full, np.ascontiguousarray(irf.copy()), x, 13.0, 0, n, dt)\n"
            "short = np.zeros(n)\n"
            "tttrlib.fconv_per(short, np.ascontiguousarray(irf.copy()), x, 13.0, 0, n - 1, dt)\n"
            "cs = np.zeros(n)\n"
            "tttrlib.fconv_per_cs(fit=cs, irf=np.ascontiguousarray(irf.copy()), x=x,\n"
            "                     period=13.0, conv_stop=n - 1, stop=n - 1, dt=dt)\n"
            "print(np.abs(full - cs).max(), np.abs(short - cs)[:-1].max(),"
            " np.abs(short - cs)[-1])\n"
        )
        for label, env in (("simd", {"TTTRLIB_USE_NEON": "1", "TTTRLIB_USE_AVX": "1"}),
                           ("scalar", {"TTTRLIB_USE_NEON": "0", "TTTRLIB_USE_AVX": "0"})):
            with self.subTest(kernel=label):
                full_vs_cs, short_vs_cs_body, short_vs_cs_last = self.run_in(env, body)
                self.assertEqual(full_vs_cs, 0.0,
                                 "the two periodic kernels no longer agree over "
                                 "the full range")
                self.assertEqual(short_vs_cs_body, 0.0,
                                 "a short stop changed a bin other than the last")
                self.assertGreater(short_vs_cs_last, 0.0,
                                   "the last bin should lack its periodic tail "
                                   "when stop excludes it")

    def test_fconv_per_overwrites_on_every_path_and_every_numexp(self):
        """The regression itself: calling twice into one buffer must not double.

        `numexp == 1` is the case that was broken with no environment override
        at all, because one lifetime never reaches the SIMD kernel.
        """
        for numexp in (1, 2, 4):
            body = (
                "x = np.array([%s])\n" % ", ".join(
                    "%f, %f" % (1.0 / numexp, 2.0 + k) for k in range(numexp))
                + "fit = np.zeros(n)\n"
                "for _ in range(3):\n"
                "    tttrlib.fconv_per(fit=fit, irf=irf, x=x, period=13.0,"
                " start=0, stop=-1, dt=0.2)\n"
                "once = np.zeros(n)\n"
                "tttrlib.fconv_per(fit=once, irf=irf, x=x, period=13.0,"
                " start=0, stop=-1, dt=0.2)\n"
                "print(fit.sum(), once.sum())\n"
            )
            for label, env in (("simd", {"TTTRLIB_USE_NEON": "1", "TTTRLIB_USE_AVX": "1"}),
                               ("scalar", {"TTTRLIB_USE_NEON": "0", "TTTRLIB_USE_AVX": "0"})):
                with self.subTest(numexp=numexp, kernel=label):
                    three, one = self.run_in(env, body)
                    self.assertAlmostEqual(
                        three, one, places=9,
                        msg="three calls into one buffer != one call: the %s "
                            "kernel is accumulating, not overwriting" % label)


if __name__ == '__main__':
    # Run tests with verbose output
    unittest.main(verbosity=2)
