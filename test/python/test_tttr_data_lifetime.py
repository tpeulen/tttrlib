"""A view of a TTTR's columns keeps the TTTR alive (it read freed memory once the TTTR was gone)."""

import gc

import numpy as np

import tttrlib


def _stream(seed, n=200_000):
    rng = np.random.default_rng(seed)
    return tttrlib.TTTR(
        np.cumsum(rng.integers(1, 50, n)).astype(np.uint64),
        rng.integers(0, 4096, n).astype(np.uint16),
        rng.integers(0, 4, n).astype(np.int8),
        np.zeros(n, dtype=np.int8),
    )


def test_a_column_view_outlives_its_tttr():
    tttr = _stream(1)
    expected = {name: np.asarray(getattr(tttr, getter)).copy() for name, getter in (
        ("macro_time", "macro_times"), ("micro_time", "micro_times"),
        ("routing_channel", "routing_channels"))}
    views = {name: tttr.data()[name].numpy() for name in expected}
    del tttr
    gc.collect()
    _reuse = [_stream(seed) for seed in range(5)]  # would land in the freed buffers
    for name, values in expected.items():
        np.testing.assert_array_equal(views[name], values)


def test_the_view_is_zero_copy_and_equals_the_getter():
    tttr = _stream(2)
    view = tttr.data()["micro_time"].numpy()
    assert not view.flags.owndata
    np.testing.assert_array_equal(view, tttr.micro_times)
