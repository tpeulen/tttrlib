"""A long tttrlib call must not stall other Python threads.

The wrapper is generated with SWIG ``-threads``, so the GIL is released
around every C++ call. A worker
thread runs a long computation while the main thread keeps a heartbeat
alive, and the heartbeat must not stall for the duration of the call --
which is exactly what happens when the GIL is held.
"""

import threading
import time

import numpy as np
import pytest

tttrlib = pytest.importorskip("tttrlib")


def _long_call(n_patterns):
    """A CPU-bound C++ call whose duration scales with the number of patterns."""
    n = 4096
    x = np.linspace(0.0, 20.0, n)
    patterns = [np.exp(-x / tau).tolist() for tau in np.linspace(0.5, 8.0, n_patterns)]
    data = np.random.poisson(5e3 * np.exp(-x / 4.0) + 5.0).astype(float).tolist()
    return lambda: tttrlib.decay_pattern_fit(
        data, patterns, tttrlib.PatternFitMode_kTikhonov, 1e-3, 10 * n_patterns, 1e-10)


def test_a_long_call_does_not_stall_the_heartbeat():
    np.random.seed(0)
    # Scale the work up until one call takes long enough to be meaningful on
    # this machine; the heartbeat assertion needs a call that DOMINATES the
    # tolerated gap.
    call, duration = None, 0.0
    for n_patterns in (100, 200, 400, 800):
        call = _long_call(n_patterns)
        t0 = time.perf_counter()
        call()
        duration = time.perf_counter() - t0
        if duration >= 0.4:
            break
    if duration < 0.4:
        pytest.skip("machine too fast to make the call long enough")

    gaps = []
    done = threading.Event()

    def worker():
        call()
        done.set()

    thread = threading.Thread(target=worker)
    last = time.perf_counter()
    thread.start()
    while not done.is_set():
        time.sleep(0.005)
        now = time.perf_counter()
        gaps.append(now - last)
        last = now
    thread.join()

    max_gap = max(gaps)
    # With the GIL held for the whole call the main thread beats once, after
    # the call: max_gap ~= duration. Released, it beats every few ms all the
    # way through. Half the duration is a loose bound that still separates
    # the two outcomes decisively.
    assert max_gap < 0.5 * duration, (
        f"heartbeat stalled for {max_gap:.3f}s during a {duration:.3f}s call "
        "-- the GIL is not released"
    )
