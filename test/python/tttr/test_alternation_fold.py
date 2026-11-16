"""µs-ALEX alternation fold: the folded clock overlays the excitation slots.

Reference: the numpy fold of the BmrA µs-ALEX H2MM analysis,
``t' = floor(t / P) * P/2 + (t mod P) mod P/2``, which the equal two-slot fold
reproduces up to its constant origin of one folded period.
"""

from __future__ import annotations

import numpy as np
import pytest
import tttrlib

P = 8000


def alex_stream(n=20000, shift=0, seed=3, windows=((578, 3782), (4278, 7762))):
    """A µs-ALEX stream: Poisson arrivals, each photon inside one of the excitation windows.

    Returns the TTTR (micro times = alternation phases, header = ALEX contract), the raw
    macro times and the window index (excitation) of every photon.
    """
    rng = np.random.default_rng(seed)
    t = np.cumsum(rng.integers(1, 400, n)).astype(np.uint64) + 10**6
    phase = (t.astype(np.int64) - shift) % P
    which = np.full(n, -1)
    for k, (a, b) in enumerate(windows):
        which[(phase >= a) & (phase <= b)] = k
    keep = which >= 0
    t, phase, which = t[keep], phase[keep], which[keep]
    stream = tttrlib.TTTR()
    stream.append_events(t, phase.astype(np.uint16), which.astype(np.int8),
                         np.zeros(t.size, np.int8), False, 0)
    stream.header.set_macro_time_resolution(12.5e-9)
    stream.header.set_micro_time_resolution(12.5e-9)
    stream.header.set_number_of_micro_time_channels(P)
    return stream, t, which


def test_header_contract_gives_the_period():
    stream, _, _ = alex_stream()
    assert stream.get_alternation_period() == P
    stream.header.set_micro_time_resolution(16e-12)
    assert stream.get_alternation_period() == 0


def test_default_fold_is_the_reference_half_period_fold():
    stream, t, _ = alex_stream()
    t = t.astype(np.int64)
    expected = (t // P) * (P // 2) + (t % P) % (P // 2) + P // 2
    np.testing.assert_array_equal(np.asarray(stream.get_folded_macro_times(), np.int64), expected)


def test_fold_from_windows_handles_a_period_shift():
    # 2026-11 containers: shift 4000 puts the donor window in the second half of the phase
    windows = ((4616, 7784), (280, 3800))
    stream, t, _ = alex_stream(shift=4000, windows=windows)
    fold = tttrlib.AlternationFold.from_windows(P, [4616, 7784, 280, 3800])
    assert fold.tick_scale == 2.0
    folded = np.asarray(stream.get_folded_macro_times(fold), np.int64)
    raw = np.asarray(fold.fold_macro_times(t, 4000), np.int64)
    np.testing.assert_array_equal(folded, raw)
    # one period of real time is one slot length of folded time
    span_real = int(t[-1]) - int(t[0])
    span_folded = int(folded.max()) - int(folded.min())
    assert abs(span_folded * fold.tick_scale - span_real) < 2 * P


def test_folded_clock_overlays_both_excitations():
    """A window of a quarter period sees one laser on the raw clock, both on the folded one."""
    stream, t, which = alex_stream(n=200000)
    folded = np.asarray(stream.get_folded_macro_times(), np.int64)

    def mixed_fraction(times):
        bins = times // (P // 4)
        mixed = 0
        groups = np.split(which[np.argsort(times, kind="stable")],
                          np.flatnonzero(np.diff(np.sort(bins))) + 1)
        for g in groups:
            if g.size >= 4:
                mixed += int(np.unique(g).size == 2)
        return mixed / max(len(groups), 1)

    assert mixed_fraction(t.astype(np.int64)) < 0.02
    assert mixed_fraction(folded) > 0.5


def test_tcspc_stream_refuses_the_default_fold():
    stream, _, _ = alex_stream()
    stream.header.set_micro_time_resolution(16e-12)
    with pytest.raises((ValueError, RuntimeError)):
        stream.get_folded_macro_times()


def test_phases_outside_the_period_are_refused():
    stream, _, _ = alex_stream()
    with pytest.raises((ValueError, RuntimeError)):
        stream.get_folded_macro_times(tttrlib.AlternationFold.equal_slots(4000, 2, 0))


def test_aligned_windows_share_a_common_folded_window():
    """Gates of unequal length are centre-cropped to one folded interval."""
    fold = tttrlib.AlternationFold.from_windows(P, [4616, 7784, 280, 3800])
    assert fold.common_window_length == 3169
    lo_delayed = 280 + (3521 - 3169) // 2
    t = 8000 * 1234
    assert fold.fold(t + lo_delayed, lo_delayed) == fold.fold(t + 4616, 4616)
    assert fold.in_common_window(lo_delayed) and not fold.in_common_window(lo_delayed - 1)
    assert tttrlib.AlternationFold.equal_slots(P, 2, 0).common_window_length == 0
