"""Simultaneous closing/opening markers retain real frame photons and microtimes."""

import numpy as np
import pytest
import tttrlib


def scanner_stream(opening_order):
    """Build two complete frames with explicit line stops and no pixel markers."""
    macro, micro, channels, events = [], [], [], []
    expected_indices = {}
    expected_means = np.empty((2, 2, 4), dtype=float)

    def append(tick, channel, event, tac=0):
        index = len(macro)
        macro.append(tick)
        micro.append(tac)
        channels.append(channel)
        events.append(event)
        return index

    for frame in range(2):
        for line in range(2):
            start = (frame * 2 + line) * 4
            if line == 0:
                for channel in opening_order:
                    append(start, channel, 1)
            else:
                append(start, 1, 1)
            for pixel in range(4):
                tac = frame * 20 + line * 8 + pixel * 2 + 3
                expected_indices[frame, line, pixel] = [
                    append(start + pixel, 0, 0, tac),
                    append(start + pixel, 0, 0, tac + 2),
                ]
                expected_means[frame, line, pixel] = tac + 1
            # At frame boundaries this stop precedes the next frame/start
            # markers in the same tick, just as an ordinary raster scan does.
            append(start + 4, 2, 1)
    stream = tttrlib.TTTR(
        np.asarray(macro, dtype=np.uint64),
        np.asarray(micro, dtype=np.uint16),
        np.asarray(channels, dtype=np.int8),
        np.asarray(events, dtype=np.int8),
    )
    stream.header.set_macro_time_resolution(20e-6)
    stream.header.set_micro_time_resolution(1e-9)
    stream.header.set_number_of_micro_time_channels(64)
    return stream, expected_indices, expected_means


@pytest.mark.parametrize("opening_order", [(4, 1), (1, 4)])
@pytest.mark.parametrize("build_pixels", [False, True])
def test_explicit_stop_before_simultaneous_frame_start_preserves_counts_and_microtime(
    opening_order, build_pixels
):
    """Closing the old frame must not create zero-duration stop/start pairs in the new frame."""
    stream, expected_indices, expected_means = scanner_stream(opening_order)
    image = tttrlib.CLSMImage(
        tttr_data=stream,
        channels=[0],
        marker_frame_start=[4],
        marker_line_start=1,
        marker_line_stop=2,
        marker_event_type=1,
        n_pixel_per_line=4,
        reading_routine="default",
        skip_before_first_frame_marker=True,
        skip_after_last_frame_marker=False,
        use_pixel_markers=False,
        settings={"n_lines": 2, "build_pixels": build_pixels},
        fill=False,
    )
    assert tuple(image.shape) == (2, 2, 4)
    image.fill(stream, channels=[0])
    expected_counts = np.full((2, 2, 4), 2, dtype=np.uint32)
    np.testing.assert_array_equal(image.get_intensity_u32(), expected_counts)
    means = image.get_mean_micro_time(
        stream,
        microtime_resolution=1.0,
        minimum_number_of_photons=0,
        stack_frames=False,
        correct_irf_offset=False,
    )
    np.testing.assert_array_equal(means, expected_means)
    # Force the lazy pixel handles and verify the actual input identities,
    # not merely equal image totals after photons moved across frame edges.
    actual_indices = []
    for (frame, line, pixel), expected in expected_indices.items():
        actual = image[frame][line][pixel].tttr_indices
        np.testing.assert_array_equal(actual, expected)
        actual_indices.extend(actual)
    assert len(actual_indices) == len(set(actual_indices)) == 32
    np.testing.assert_array_equal(image.get_intensity_u32(), expected_counts)


def test_old_public_frame_edge_positional_and_keyword_calls_are_retained():
    """The C++-only overload does not disable the existing Python keyword-callable helper."""
    stream, _, _ = scanner_stream((1, 4))
    old_edges = tttrlib.CLSMImage.get_frame_edges(stream, 0, -1, [4], 1, 0, True, False, 1, 2)
    assert len(old_edges) == 3
    named = tttrlib.CLSMImage.get_frame_edges(
        tttr=stream,
        start_event=0,
        stop_event=-1,
        marker_frame_start=[4],
        marker_event_type=1,
        reading_routine=0,
        skip_before_first_frame_marker=True,
        skip_after_last_frame_marker=False,
        marker_line_start=1,
        expected_n_lines=2,
    )
    np.testing.assert_array_equal(named, old_edges)


@pytest.mark.parametrize("line_stop", [1, 255])
@pytest.mark.parametrize("opening_order", [(4, 1), (1, 4)])
def test_shared_and_start_only_line_clocks_keep_existing_reconstruction(line_stop, opening_order):
    """Two start pulses delimit one usable line without a distinct closing-marker barrier."""
    stream, expected_indices, _ = scanner_stream(opening_order)
    image = tttrlib.CLSMImage(
        tttr_data=stream,
        channels=[0],
        marker_frame_start=[4],
        marker_line_start=1,
        marker_line_stop=line_stop,
        marker_event_type=1,
        n_pixel_per_line=4,
        reading_routine="default",
        use_pixel_markers=False,
        skip_before_first_frame_marker=True,
        skip_after_last_frame_marker=False,
        settings={"n_lines": 1},
        fill=True,
    )
    np.testing.assert_array_equal(image.get_intensity_u32(), np.full((2, 1, 4), 2))
    for frame in range(2):
        for pixel in range(4):
            np.testing.assert_array_equal(
                image[frame][0][pixel].tttr_indices, expected_indices[frame, 0, pixel]
            )
