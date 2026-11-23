"""The multi-tau (wahl) lag axis labels each bin with the lag its pairs hold.

Two streams whose partner photons are exactly D ticks apart (random phase
relative to the coarse grid) put all counts into the bins around D; the
count-weighted mean label must be D. Before 2026-11-23 the labels of cascades
>= 4 (n_bins = 8) were up to one coarse bin too high (6-10 % in lag), because
the kernel counts integer coarse lags offset + m with offset = x[i*n_bins] >> i
while the axis accumulated the exact, non-integer offset.
"""
import numpy as np
import pytest

import tttrlib


def _probe(delay, n_bins, n=20000, seed=0):
    rng = np.random.default_rng(seed)
    base = np.sort(np.arange(n, dtype=np.uint64) * np.uint64(10 * delay + 100000)
                   + rng.integers(0, 4096, n).astype(np.uint64))
    c = tttrlib.Correlator()
    c.n_bins, c.n_casc = n_bins, 22
    c.method = "wahl"
    c.set_events(base, np.ones(n), base + np.uint64(delay), np.ones(n))
    x = np.asarray(c.x_axis)
    y = np.asarray(c.get_corr())
    near = (x > 0.5 * delay) & (x < 1.5 * delay) & (y > 0)
    return np.sum(x[near] * y[near]) / np.sum(y[near])


@pytest.mark.parametrize("n_bins", [8, 16])
@pytest.mark.parametrize("delay", [37, 100, 333, 1000, 3000, 12345, 100000])
def test_mean_label_is_the_delay(delay, n_bins):
    assert _probe(delay, n_bins) == pytest.approx(delay, rel=0.01)


@pytest.mark.parametrize("n_bins", [4, 8, 10, 16])
def test_labels_of_cascade_i_are_multiples_of_its_spacing(n_bins):
    c = tttrlib.Correlator()
    c.n_bins, c.n_casc = n_bins, 18
    x = np.asarray(c.x_axis).astype(np.int64)
    assert np.all(np.diff(x) > 0)
    for j in range(1, len(x)):
        i = (j - 1) // n_bins
        assert x[j] % (1 << i) == 0, (j, x[j], i)
