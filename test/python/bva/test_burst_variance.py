#!/usr/bin/env python3
r"""BurstVariance -- the slicing engine behind BVA and SVA, on synthetic photons.

A/B against a NumPy transcription of the definitions in ``BurstVariance.h``:
slices of A ∪ B photons (fixed photon chunks without the trailing partial one;
fixed time windows from the earliest photon, optionally aligned to whole periods),
per-slice ratio n_A / N, SD with ``ddof``, pooled ratio, binomial chi2.
The static bound is checked distributionally (a static burst lands below its
q-bound with probability q) and for determinism.
"""
import unittest

import numpy as np

import tttrlib


def _stream(n_photons, rng, p_a=0.4, gap=1_000_000, burst_len=200, rate=3):
    """A stream of bursts: channel 0 = B, 1 = A, 2 = neither; steady arrival."""
    macro, chan, bounds = [], [], []
    t = 0
    while len(macro) < n_photons:
        start = len(macro)
        for _ in range(burst_len):
            t += int(rng.integers(1, rate + 1))
            macro.append(t)
            u = rng.random()
            chan.append(2 if u < 0.1 else (1 if rng.random() < p_a else 0))
        bounds.append((start, len(macro) - 1))
        t += gap
    d = tttrlib.TTTR()
    d.append_events(np.asarray(macro, np.uint64), np.zeros(len(macro), np.uint16),
                    np.asarray(chan, np.int8), np.zeros(len(macro), np.int8), False, 0)
    return d, np.asarray(macro, np.int64), np.asarray(chan), np.asarray(bounds, np.int64)


def _reference(axis, a, b, bounds, mode, n, w, align, min_photons, min_slices, ddof):
    out = []
    for s, e in bounds:
        idx = np.arange(s, e + 1)
        idx = idx[a[idx] | b[idx]]
        slices = []
        if mode == 0:
            for k in range(idx.size // n):
                sl = idx[k * n:(k + 1) * n]
                slices.append((int(a[sl].sum()), sl.size))
        else:
            t = axis[idx].astype(float)
            t0, t1 = t.min(), t.max()
            if align > 0:
                t0, t1 = (np.floor(t0 / align) + 1) * align, np.floor(t1 / align) * align
            k = int(np.floor((t1 - t0) / w)) if t1 > t0 else 0
            win = np.floor((t - t0) / w)
            for j in range(k):
                sl = idx[win == j]
                slices.append((int(a[sl].sum()), sl.size))
        slices = [x for x in slices if x[1] >= min_photons]
        if len(slices) < min_slices:
            out.append((np.nan, np.nan, np.nan, np.nan, len(slices)))
            continue
        sa = np.array([x[0] for x in slices], float)
        sn = np.array([x[1] for x in slices], float)
        r = sa / sn
        p = sa.sum() / sn.sum()
        chi2 = np.sum((sa - sn * p) ** 2 / (sn * p * (1 - p))) if 0 < p < 1 else np.nan
        sd = r.std(ddof=ddof) if r.size > ddof else np.nan
        out.append((r.mean(), sd, p, chi2, len(slices)))
    return np.array(out, float)


class TestBurstVariance(unittest.TestCase):
    def setUp(self):
        self.rng = np.random.default_rng(3)
        self.tttr, self.macro, self.chan, self.bounds = _stream(20_000, self.rng)
        self.a = self.chan == 1
        self.b = self.chan == 0

    def _engine(self):
        bv = tttrlib.BurstVariance(self.tttr)
        bv.set_stream_a([1])
        bv.set_stream_b([0])
        return bv

    def _check(self, bv, ref):
        np.testing.assert_allclose(bv.mean, ref[:, 0], rtol=0, atol=1e-12, equal_nan=True)
        np.testing.assert_allclose(bv.sd, ref[:, 1], rtol=0, atol=1e-12, equal_nan=True)
        np.testing.assert_allclose(bv.ratio, ref[:, 2], rtol=0, atol=1e-12, equal_nan=True)
        np.testing.assert_allclose(bv.chi2, ref[:, 3], rtol=1e-12, atol=1e-12, equal_nan=True)
        np.testing.assert_array_equal(bv.n_slices, ref[:, 4].astype(int))

    def test_photon_slices_match_reference(self):
        for ddof in (0, 1):
            bv = self._engine()
            bv.compute(self.bounds, bv.PHOTONS, 7, 0.0, 0.0, 0.0, 1, 2, ddof)
            ref = _reference(self.macro, self.a, self.b, self.bounds, 0, 7, 0, 0, 1, 2, ddof)
            self._check(bv, ref)

    def test_time_windows_match_reference(self):
        for align in (0.0, 25.0):
            bv = self._engine()
            bv.set_time_axis(self.macro)
            bv.compute(self.bounds, bv.TIME, 0, 50.0, align, 0.0, 5, 2, 1)
            ref = _reference(self.macro, self.a, self.b, self.bounds, 1, 0, 50.0, align, 5, 2, 1)
            self._check(bv, ref)

    def test_masks_equal_streams(self):
        bv = self._engine()
        bv.compute(self.bounds, bv.PHOTONS, 5, 0.0, 0.0, 0.0, 1, 2, 1)
        bm = tttrlib.BurstVariance(self.tttr)
        bm.set_masks(self.a.astype(np.uint8), self.b.astype(np.uint8))
        bm.compute(self.bounds, bm.PHOTONS, 5, 0.0, 0.0, 0.0, 1, 2, 1)
        np.testing.assert_array_equal(bv.sd, bm.sd)
        np.testing.assert_array_equal(bv.slice_photons, bm.slice_photons)

    def test_slice_arrays_align(self):
        bv = self._engine()
        bv.compute(self.bounds, bv.PHOTONS, 5, 0.0, 0.0, 0.0, 1, 2, 1)
        owner, n, a = bv.slice_owner, bv.slice_photons, bv.slice_a
        self.assertTrue(np.all(np.diff(owner) >= 0))
        self.assertTrue(np.all(n == 5))
        pooled = np.bincount(owner, weights=a, minlength=len(self.bounds)) / np.bincount(
            owner, weights=n, minlength=len(self.bounds))
        np.testing.assert_allclose(pooled[bv.tested], bv.ratio[bv.tested], atol=1e-12)

    def test_static_bound_coverage_and_determinism(self):
        bv = self._engine()
        bv.compute(self.bounds, bv.PHOTONS, 10, 0.0, 0.0, 0.0, 1, 2, 1)
        bound = bv.static_bound(0.9, 400, 11, 1.0, 1)
        np.testing.assert_array_equal(bound, bv.static_bound(0.9, 400, 11, 1.0, 1))
        cls = tttrlib.BurstVariance.classify(bv.sd, bound)
        ok = cls >= 0
        # static photons: about 90 % of the tested bursts inside their 90 % bound
        self.assertGreater(ok.sum(), 50)
        self.assertAlmostEqual((cls[ok] == 1).mean(), 0.9, delta=0.06)
        # the bound scales with sqrt(dispersion)
        np.testing.assert_allclose(bv.static_bound(0.9, 400, 11, 4.0, 1), 2 * bound)

    def test_dynamic_bursts_exceed_bound(self):
        # each burst switches between p_a = 0.1 and 0.8 every 50 photons
        macro, chan, bounds = [], [], []
        t = 0
        for _ in range(60):
            start = len(macro)
            for k in range(200):
                t += 1
                macro.append(t)
                p = 0.1 if (k // 50) % 2 == 0 else 0.8
                chan.append(1 if self.rng.random() < p else 0)
            bounds.append((start, len(macro) - 1))
            t += 100_000
        d = tttrlib.TTTR()
        d.append_events(np.asarray(macro, np.uint64), np.zeros(len(macro), np.uint16),
                        np.asarray(chan, np.int8), np.zeros(len(macro), np.int8), False, 0)
        bv = tttrlib.BurstVariance(d)
        bv.set_stream_a([1])
        bv.set_stream_b([0])
        bv.compute(np.asarray(bounds, np.int64), bv.PHOTONS, 10, 0.0, 0.0, 0.0, 1, 2, 1)
        cls = tttrlib.BurstVariance.classify(bv.sd, bv.static_bound(0.95, 300, 11, 1.0, 1))
        self.assertGreater((cls == 0).mean(), 0.9)

    def test_untested_and_shot_noise(self):
        bv = self._engine()
        bv.compute(self.bounds, bv.PHOTONS, 1000, 0.0, 0.0, 0.0, 1, 2, 1)  # no burst has 2 slices
        self.assertFalse(bv.tested.any())
        self.assertTrue(np.all(tttrlib.BurstVariance.classify(bv.sd, bv.static_bound()) == -1))
        p = np.array([0.2, 0.5])
        np.testing.assert_allclose(tttrlib.BurstVariance.shot_noise_sd(p, [10.0]),
                                   np.sqrt(p * (1 - p) / 10))

    def test_anisotropy_statistic(self):
        g, l1, l2 = 1.1, 0.03, 0.02
        bv = self._engine()
        bv.set_statistic(bv.ANISOTROPY, g, l1, l2)
        bv.compute(self.bounds, bv.PHOTONS, 8, 0.0, 0.0, 0.0, 1, 2, 1)
        ok = bv.tested
        owner, a, n = bv.slice_owner, bv.slice_a.astype(float), bv.slice_photons.astype(float)
        b = n - a
        r = (a - g * b) / ((1 - 3 * l2) * a + (2 - 3 * l1) * g * b)
        mean = np.array([r[owner == i].mean() for i in np.flatnonzero(ok)])
        sd = np.array([r[owner == i].std(ddof=1) for i in np.flatnonzero(ok)])
        np.testing.assert_allclose(bv.mean[ok], mean, atol=1e-12)
        np.testing.assert_allclose(bv.sd[ok], sd, atol=1e-12)
        sa = np.bincount(owner, weights=a)[ok]
        sb = np.bincount(owner, weights=b)[ok]
        np.testing.assert_allclose(bv.pooled[ok], (sa - g * sb) / ((1 - 3 * l2) * sa + (2 - 3 * l1) * g * sb),
                                   atol=1e-12)
        # static photons: the anisotropy bound covers them at its quantile, too
        cls = tttrlib.BurstVariance.classify(bv.sd, bv.static_bound(0.9, 400, 11, 1.0, 1))
        self.assertAlmostEqual((cls[cls >= 0] == 1).mean(), 0.9, delta=0.06)

    def test_slice_durations(self):
        bv = self._engine()
        bv.compute(self.bounds, bv.PHOTONS, 5, 0.0, 0.0, 0.0, 1, 2, 1)
        d = bv.slice_duration
        self.assertEqual(d.size, bv.slice_photons.size)
        expected = []
        for s_, e_ in self.bounds:
            idx = np.arange(s_, e_ + 1)
            idx = idx[self.a[idx] | self.b[idx]]
            k = idx.size // 5
            if k >= 2:
                for j in range(k):
                    t = self.macro[idx[j * 5:(j + 1) * 5]]
                    expected.append(t.max() - t.min())
        np.testing.assert_array_equal(d, np.asarray(expected, float))
        bv.set_time_axis(self.macro)
        bv.compute(self.bounds, bv.TIME, 0, 50.0, 0.0, 0.0, 1, 2, 1)
        np.testing.assert_array_equal(bv.slice_duration, 50.0)

    def test_background_widens_the_bound(self):
        bv = self._engine()
        bv.compute(self.bounds, bv.PHOTONS, 10, 0.0, 0.0, 0.0, 1, 2, 1)
        plain = bv.static_bound(0.95, 300, 11, 1.0, 1)
        none = bv.static_bound(0.95, 300, 11, 1.0, 1, 0.0, 0.0)
        np.testing.assert_array_equal(plain, none)
        # background all in B, at a rate comparable to the signal: slices of different length
        # mix different background fractions, so a static burst scatters more
        mixed = bv.static_bound(0.95, 300, 11, 1.0, 1, 0.0, 0.2)
        ok = np.isfinite(plain)
        self.assertGreater(np.median(mixed[ok] / plain[ok]), 1.0)

    def test_rejects_bad_arguments(self):
        bv = self._engine()
        with self.assertRaises(Exception):
            bv.compute(self.bounds, bv.TIME, 0, 0.0, 0.0, 0.0, 1, 2, 1)
        with self.assertRaises(Exception):
            bv.compute(self.bounds, bv.PHOTONS, 0, 0.0, 0.0, 0.0, 1, 2, 1)
        bv.set_time_axis(self.macro[:10])
        with self.assertRaises(Exception):
            bv.compute(self.bounds, bv.TIME, 0, 10.0, 0.0, 0.0, 1, 2, 1)


if __name__ == "__main__":
    unittest.main()
