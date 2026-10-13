"""CLSMImage.get_record_kinds / classify_records(_events): what each TTTR record is.

A record is a photon, or a pixel, line-start, line-stop, frame or other marker,
named by the same marker settings (and the same SP8 micro-time matching) the
image reconstruction uses. The synthetic streams carry the expected kind of
every record; the real files are checked against the marker channels their
header and reading routine imply, counted independently with numpy.
"""

import os

import numpy as np
import pytest
import tttrlib

PHOTON = tttrlib.CLSM_RECORD_PHOTON
PIXEL = tttrlib.CLSM_RECORD_PIXEL_MARKER
LINE_START = tttrlib.CLSM_RECORD_LINE_START_MARKER
LINE_STOP = tttrlib.CLSM_RECORD_LINE_STOP_MARKER
FRAME = tttrlib.CLSM_RECORD_FRAME_MARKER
OTHER = tttrlib.CLSM_RECORD_OTHER_MARKER

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.getenv("TTTRLIB_DATA") or os.path.join(HERE, "..", "..", "..", "tttr-data")


def test_kind_codes_are_distinct_and_photon_is_zero():
    codes = [PHOTON, PIXEL, LINE_START, LINE_STOP, FRAME, OTHER]
    assert PHOTON == 0
    assert len(set(codes)) == 6


def _stream(records):
    """A TTTR from (channel, event, micro) triples, one macro tick apart."""
    channels = np.array([r[0] for r in records], dtype=np.int8)
    events = np.array([r[1] for r in records], dtype=np.int8)
    micro = np.array([r[2] for r in records], dtype=np.uint16)
    macro = np.arange(len(records), dtype=np.uint64)
    return tttrlib.TTTR(macro, micro, channels, events)


def test_default_routine_names_markers_by_routing_channel():
    # frame=4, line start=2, line stop=3, pixel=1, an unnamed marker on 7,
    # and a photon on channel 2 that must stay a photon.
    records = [(4, 1, 0), (2, 1, 0), (1, 1, 0), (0, 0, 5), (2, 0, 9), (1, 1, 0),
               (3, 1, 0), (7, 1, 0)]
    expected = [FRAME, LINE_START, PIXEL, PHOTON, PHOTON, PIXEL, LINE_STOP, OTHER]
    settings = tttrlib.CLSMSettings(
        marker_frame_start=[4], marker_line_start=2, marker_line_stop=3,
        marker_event_type=1, use_pixel_markers=True, marker_pixel=1,
    )
    kinds = tttrlib.CLSMImage.classify_records(_stream(records), settings)
    assert kinds.dtype == np.int8
    assert kinds.tolist() == expected


def test_pixel_markers_are_other_without_pixel_binning():
    records = [(1, 1, 0), (0, 0, 3)]
    settings = tttrlib.CLSMSettings(
        marker_frame_start=[4], marker_line_start=2, marker_line_stop=3,
        marker_event_type=1, use_pixel_markers=False,
    )
    kinds = tttrlib.CLSMImage.classify_records(_stream(records), settings)
    assert kinds.tolist() == [OTHER, PHOTON]


def test_start_only_line_clock_has_no_line_stop():
    # BH SPC-130: marker_line_stop is the no-stop sentinel 255.
    records = [(2, 1, 0), (0, 0, 1)]
    settings = tttrlib.CLSMSettings(
        marker_frame_start=[4], marker_line_start=2, marker_line_stop=255,
        marker_event_type=1,
    )
    assert tttrlib.CLSMImage.classify_records(_stream(records), settings).tolist() == [
        LINE_START, PHOTON]


def test_sp8_routine_reads_marker_bits_from_the_micro_time():
    # Leica SP8 / PicoHarp T3: every marker on routing channel 15, the marker
    # bits in the micro time (1 line start, 2 line stop, 4 frame).
    records = [(15, 1, 4), (15, 1, 1), (0, 0, 4), (15, 1, 2), (15, 1, 8), (3, 1, 1)]
    expected = [FRAME, LINE_START, PHOTON, LINE_STOP, OTHER, OTHER]
    settings = tttrlib.CLSMSettings(
        marker_frame_start=[4, 6], marker_line_start=1, marker_line_stop=2,
        marker_event_type=15, reading_routine=tttrlib.CLSM_SP8,
    )
    assert tttrlib.CLSMImage.classify_records(_stream(records), settings).tolist() == expected


def test_events_form_equals_the_tttr_form():
    records = [(4, 1, 0), (2, 1, 0), (0, 0, 5), (3, 1, 0), (9, 1, 0)]
    stream = _stream(records)
    settings = tttrlib.CLSMSettings(
        marker_frame_start=[4], marker_line_start=2, marker_line_stop=3, marker_event_type=1,
    )
    events = tttrlib.CLSMImage.classify_records_events(
        np.asarray(stream.event_types, dtype=np.int8),
        np.asarray(stream.routing_channels, dtype=np.int8),
        np.asarray(stream.micro_times, dtype=np.uint16),
        settings,
    )
    assert events.tolist() == tttrlib.CLSMImage.classify_records(stream, settings).tolist()


def test_unnamed_settings_call_every_marker_other():
    records = [(4, 1, 0), (2, 1, 0), (0, 0, 5)]
    settings = tttrlib.CLSMSettings(
        marker_frame_start=[], marker_line_start=-1, marker_line_stop=-1,
        marker_event_type=1, use_pixel_markers=False,
    )
    assert tttrlib.CLSMImage.classify_records(_stream(records), settings).tolist() == [
        OTHER, OTHER, PHOTON]


def _file(*parts):
    path = os.path.join(DATA, *parts)
    if not os.path.exists(path):
        pytest.skip(f"test data not available: {path}")
    return path


def _counts_by_channel(tttr, channels):
    et = np.asarray(tttr.event_types)
    rc = np.asarray(tttr.routing_channels)
    return int(np.count_nonzero((et == 1) & np.isin(rc, channels)))


def test_bh_pixel_clock_scan_names_every_marker_from_the_header():
    """BH SPC-130 with SP_PIX_CLK: pixel 1, line start 2 (no stop), frame 4."""
    tttr = tttrlib.TTTR(_file("imaging", "bh", "spcm", "FocalCheck_A1_20x_8xzoom_750nm_m1.spc"))
    image = tttrlib.CLSMImage(tttr, build_pixels=False)
    kinds = np.asarray(image.get_record_kinds())
    et = np.asarray(tttr.event_types)
    assert len(kinds) == len(et)
    assert np.count_nonzero(kinds == PHOTON) == np.count_nonzero(et == 0)
    assert np.count_nonzero(kinds == PIXEL) == _counts_by_channel(tttr, [1])
    assert np.count_nonzero(kinds == LINE_START) == _counts_by_channel(tttr, [2])
    assert np.count_nonzero(kinds == FRAME) == _counts_by_channel(tttr, [4])
    assert np.count_nonzero(kinds == LINE_STOP) == 0
    assert np.count_nonzero(kinds == OTHER) == 0


def test_sp8_file_uses_the_micro_time_marker_bits():
    tttr = tttrlib.TTTR(_file("imaging", "leica", "sp8", "da", "G-28_C-28_S1_6_1.ptu"))
    image = tttrlib.CLSMImage(tttr, build_pixels=False)
    s = image.get_settings()
    kinds = np.asarray(image.get_record_kinds())
    rc = np.asarray(tttr.routing_channels)
    mt = np.asarray(tttr.micro_times)
    on_marker = (np.asarray(tttr.event_types) == 1) & (rc == s.marker_event_type)
    assert np.count_nonzero(kinds == FRAME) == np.count_nonzero(
        on_marker & np.isin(mt, list(s.marker_frame_start)))
    assert np.count_nonzero(kinds == LINE_START) == np.count_nonzero(
        on_marker & (mt == s.marker_line_start))
    assert np.count_nonzero(kinds == LINE_STOP) == np.count_nonzero(
        on_marker & (mt == s.marker_line_stop))
    assert np.count_nonzero(kinds == LINE_START) > 0
