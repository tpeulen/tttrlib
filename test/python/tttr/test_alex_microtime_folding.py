"""ALEX folding follows integer circular arithmetic without losing source fields."""

from __future__ import annotations

import subprocess
import sys

import numpy as np
import pytest
import tttrlib


def make_stream():
    """Return a fixture spanning the entire unsigned macrotime domain."""
    macro = np.array(
        [0, 1, 22, 23, 7999, 8000, 65535, 65536, 70000,
         2**31 - 1, 2**63 - 1, 2**63, 2**64 - 2, 2**64 - 1],
        dtype=np.uint64,
    )
    stream = tttrlib.TTTR()
    stream.append_events(
        macro,
        np.arange(macro.size, dtype=np.uint16),
        (np.arange(macro.size) % 3).astype(np.int8),
        np.zeros(macro.size, dtype=np.int8),
        False,
        0,
    )
    return stream, macro


@pytest.mark.parametrize("period", [1, 8000, 32768, 65536])
@pytest.mark.parametrize("shift", [0, 23, -23, 2**31 - 1, -(2**31)])
def test_fold_matches_unbounded_integer_reference(period, shift):
    """Signed shifts and full-width macro times obey the same modulo relation."""
    stream, input_macro = make_stream()
    before = {
        name: np.array(getattr(stream, name), copy=True)
        for name in ("macro_times", "routing_channels", "event_types")
    }
    header = stream.header.json
    native_macro = [stream.get_macro_time_at(i) for i in range(len(stream.micro_times))]
    assert native_macro == [int(macro) for macro in input_macro]
    expected = np.array(
        [(macro - shift) % period for macro in native_macro],
        dtype=np.uint16,
    )
    stream.alex_to_microtime(period, shift)
    np.testing.assert_array_equal(stream.micro_times, expected)
    for name, original in before.items():
        np.testing.assert_array_equal(getattr(stream, name), original)
    assert [stream.get_macro_time_at(i) for i in range(len(native_macro))] == native_macro
    assert stream.header.json == header


def test_shift_past_first_photon_wraps_to_end_of_period():
    """An early photon shifted below zero has circular phase 7978, not 7594."""
    stream, _ = make_stream()
    stream.alex_to_microtime(8000, 23)
    assert stream.micro_times[1] == 7978


@pytest.mark.parametrize("offset", [0, -23, -(2**63)])
def test_bulk_getter_preserves_unsigned_macrotimes(offset):
    """Bulk reads retain high bits and clamp only truly negative shifted values."""
    stream, input_macro = make_stream()
    stream << offset
    expected = np.array([max(int(time) + offset, 0) for time in input_macro], dtype=np.uint64)
    np.testing.assert_array_equal(stream.macro_times, expected)
    assert [stream.get_macro_time_at(i) for i in range(len(input_macro))] == input_macro.tolist()


@pytest.mark.parametrize("offset", [23, 2**63 - 1])
def test_bulk_getter_rejects_unsigned_offset_overflow(offset):
    """An unrepresentable cached shift raises without changing stored macro times."""
    stream, input_macro = make_stream()
    stream << offset
    with pytest.raises((OverflowError, RuntimeError), match="capacity"):
        stream.get_macro_times()
    assert [stream.get_macro_time_at(i) for i in range(len(input_macro))] == input_macro.tolist()


def test_bulk_getter_keeps_representable_positive_offset():
    """An ordinary positive display offset preserves its established addition sign."""
    stream = tttrlib.TTTR()
    stream.append_event(10, 17, 0, 0, False)
    stream << 23
    np.testing.assert_array_equal(stream.macro_times, np.array([33], dtype=np.uint64))
    assert stream.get_macro_time_at(0) == 10


@pytest.mark.parametrize("period", [0, 65537, 100000])
@pytest.mark.parametrize("populated", [False, True])
def test_unrepresentable_period_fails_before_mutation(period, populated):
    """Invalid periods raise even for empty streams and never modify any field.

    A subprocess keeps an old provider's zero-divisor crash inside this test.
    """
    script = f"""
import numpy as np
import tttrlib
stream = tttrlib.TTTR()
if {populated!r}:
    stream.append_events(np.array([1, 70000], dtype=np.uint64),
                         np.array([17, 29], dtype=np.uint16),
                         np.array([0, 1], dtype=np.int8),
                         np.zeros(2, dtype=np.int8), False, 0)
before = {{name: np.array(getattr(stream, name), copy=True)
          for name in ('macro_times', 'micro_times', 'routing_channels', 'event_types')}}
header = stream.header.json
try:
    stream.alex_to_microtime({period}, 23)
except (ValueError, RuntimeError) as exc:
    assert 'period' in str(exc).lower()
else:
    raise AssertionError('unrepresentable period did not raise')
for name, original in before.items():
    np.testing.assert_array_equal(getattr(stream, name), original)
assert stream.header.json == header
"""
    result = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, timeout=30
    )
    assert result.returncode == 0, result.stdout + result.stderr
