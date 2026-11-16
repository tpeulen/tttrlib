# SPDX-License-Identifier: BSD-3-Clause
"""Lee-filtered inter-photon-time burst search (the legacy Paris burstwise search)."""
from __future__ import annotations

import unittest

import numpy as np

import tttrlib

MACRO_TIME_RESOLUTION = 1e-9


def make_tttr(times_s):
    ticks = np.round(np.sort(np.asarray(times_s)) / MACRO_TIME_RESOLUTION).astype(np.uint64)
    n = ticks.size
    tttr = tttrlib.TTTR(ticks, np.zeros(n, dtype=np.uint16), np.zeros(n, dtype=np.int8),
                        np.zeros(n, dtype=np.int8))
    tttr.header.set_macro_time_resolution(MACRO_TIME_RESOLUTION)
    return tttr


def reference(times_s, L, m, threshold, s0, plus_minus):
    """The definition, in numpy."""
    t = np.asarray(times_s, dtype=np.float64)
    n = t.size
    dt = np.diff(t)
    win = np.lib.stride_tricks.sliding_window_view(np.pad(dt, m, mode="edge"), 2 * m + 1)
    mu, var = win.mean(axis=1), win.var(axis=1)
    below = (mu + var / (var + s0 * s0) * (dt - mu)) < threshold
    e = np.flatnonzero(np.diff(np.r_[0, below.astype(np.int8), 0]))
    a, b = e[::2], e[1::2]
    keep = (b - a + 1) >= L
    return np.column_stack([np.maximum(a[keep] - plus_minus + 1, 0),
                            np.minimum(b[keep] + plus_minus + 1, n - 1)])


class TestLeeBurstSearch(unittest.TestCase):

    def setUp(self):
        rng = np.random.default_rng(3)
        dt = rng.exponential(5e-4, 20000)                    # 2 kcps background
        for start, size in ((2000, 150), (6000, 90), (12000, 40), (15000, 300)):
            dt[start:start + size] = rng.exponential(1e-5, size)   # 100 kcps bursts
        self.times = np.cumsum(dt)
        self.tttr = make_tttr(self.times)
        # the times the library sees (rounded to the 1 ns tick)
        self.seen = np.asarray(self.tttr.macro_times, dtype=np.float64) * MACRO_TIME_RESOLUTION

    def test_matches_the_definition(self):
        kw = dict(L=60, m=30, threshold=1e-4, s0=1e-2, plus_minus=7)
        got = np.asarray(self.tttr.burst_search_lee(**kw), dtype=np.int64).reshape(-1, 2)
        ref = reference(self.seen, **kw)
        np.testing.assert_array_equal(got, ref)
        self.assertEqual(len(got), 3)                       # the 40-photon burst has a short core

    def test_registry_entry_and_dispatch_by_name(self):
        self.assertIn("lee", tttrlib.TTTR.burst_search_algorithms())
        by_name = self.tttr.burst_search_by_name("lee", L=60, m=30, threshold=1e-4, s0=1e-2,
                                                 plus_minus=7)
        direct = np.asarray(self.tttr.burst_search_lee(60, 30, 1e-4, 1e-2, 7)).reshape(-1, 2)
        np.testing.assert_array_equal(by_name, direct)

    def test_empty_and_tiny_streams(self):
        self.assertEqual(len(make_tttr([0.1]).burst_search_lee()), 0)


if __name__ == "__main__":
    unittest.main()
