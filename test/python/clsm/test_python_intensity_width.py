"""Python intensity retains bright-pixel counts and channel/frame geometry."""

import numpy as np
import pytest
import tttrlib


def scanner_stream(bright_count):
    """Construct two channels in two frames with known per-pixel photon identities."""
    macro, micro, channels, events = [], [], [], []
    expected_indices = {}
    expected = np.empty((2, 2, 2, 2), dtype=np.uint32)

    def append(tick, channel, event, count=1):
        start = len(macro)
        macro.extend([tick] * count)
        micro.extend([7] * count)
        channels.extend([channel] * count)
        events.extend([event] * count)
        return np.arange(start, start + count, dtype=np.int64)

    for frame in range(2):
        for line in range(2):
            tick = (frame * 2 + line) * 4
            if line == 0:
                append(tick, 4, 1)
            append(tick, 1, 1)
            for pixel in range(2):
                for channel in range(2):
                    count = bright_count if (frame, line, pixel, channel) == (0, 0, 0, 0) else (
                        2 + frame * 8 + line * 4 + pixel * 2 + channel
                    )
                    expected[channel, frame, line, pixel] = count
                    expected_indices[channel, frame, line, pixel] = append(
                        tick + pixel * 2 + 1, channel, 0, count
                    )
            append(tick + 4, 2, 1)

    stream = tttrlib.TTTR(
        np.asarray(macro, dtype=np.uint64),
        np.asarray(micro, dtype=np.uint16),
        np.asarray(channels, dtype=np.int8),
        np.asarray(events, dtype=np.int8),
    )
    stream.header.set_macro_time_resolution(20e-6)
    stream.header.set_micro_time_resolution(1e-9)
    stream.header.set_number_of_micro_time_channels(64)
    return stream, expected, expected_indices


def image_for(stream, split_by_channel, build_pixels):
    """Fill the actual provider using explicit scanner markers and two routing channels."""
    image = tttrlib.CLSMImage(
        tttr_data=stream,
        channels=[0, 1],
        marker_frame_start=[4],
        marker_line_start=1,
        marker_line_stop=2,
        marker_event_type=1,
        n_pixel_per_line=2,
        reading_routine="default",
        skip_before_first_frame_marker=True,
        skip_after_last_frame_marker=False,
        split_by_channel=split_by_channel,
        use_pixel_markers=False,
        build_pixels=build_pixels,
        settings={"n_lines": 2},
        fill=True,
    )
    return image


@pytest.mark.parametrize("split_by_channel", [False, True])
@pytest.mark.parametrize("build_pixels", [False, True])
@pytest.mark.parametrize("bright_count", [13, 65535, 65536, 86493])
def test_python_intensity_preserves_counts_geometry_and_exact_photon_indices(
    split_by_channel, build_pixels, bright_count
):
    """The default Python property must not wrap even though the narrow getter remains."""
    stream, channel_counts, expected_indices = scanner_stream(bright_count)
    image = image_for(stream, split_by_channel, build_pixels)
    expected = channel_counts if split_by_channel else channel_counts.sum(axis=0)
    assert tuple(image.shape) == expected.shape
    np.testing.assert_array_equal(image.intensity, expected)
    assert image.intensity.dtype == np.dtype(np.uint32)

    flat_counts = channel_counts.reshape(4, 2, 2) if split_by_channel else expected
    np.testing.assert_array_equal(image.get_intensity_u32(), flat_counts)
    np.testing.assert_array_equal(image.get_intensity(), flat_counts.astype(np.uint16))

    actual_indices = []
    for channel, frame, line, pixel in expected_indices:
        if split_by_channel:
            actual = image[channel][frame][line][pixel].tttr_indices
            wanted = expected_indices[channel, frame, line, pixel]
        else:
            if channel != 0:
                continue
            actual = image[frame][line][pixel].tttr_indices
            wanted = np.sort(np.concatenate([
                expected_indices[0, frame, line, pixel],
                expected_indices[1, frame, line, pixel],
            ]))
        np.testing.assert_array_equal(actual, wanted)
        actual_indices.extend(actual)
    assert len(actual_indices) == len(set(actual_indices)) == int(expected.sum())
    np.testing.assert_array_equal(image.intensity, expected)


@pytest.mark.parametrize("bright_count", [13, 86493])
def test_channel_slice_intensity_preserves_counts_and_shape(bright_count):
    """Selecting a channel keeps the wide counters and existing frames-first slice shape."""
    stream, expected, _ = scanner_stream(bright_count)
    image = image_for(stream, split_by_channel=True, build_pixels=False)
    for channel in range(2):
        channel_image = image[channel]
        assert tuple(channel_image.shape) == (2, 2, 2)
        np.testing.assert_array_equal(channel_image.intensity, expected[channel])
        assert channel_image.intensity.dtype == np.dtype(np.uint32)
    np.testing.assert_array_equal(image[-1].intensity, expected[-1])


@pytest.mark.parametrize("split_by_channel", [False, True])
@pytest.mark.parametrize("build_pixels", [False, True])
def test_explicit_wide_and_legacy_narrow_getters_keep_their_existing_contracts(
    split_by_channel, build_pixels
):
    """The existing wide getter counts every photon while the explicit legacy getter wraps."""
    stream, channel_counts, _ = scanner_stream(86493)
    image = image_for(stream, split_by_channel, build_pixels)
    expected = (
        channel_counts.reshape(4, 2, 2)
        if split_by_channel else channel_counts.sum(axis=0)
    )
    wide = image.get_intensity_u32()
    narrow = image.get_intensity()
    np.testing.assert_array_equal(wide, expected)
    np.testing.assert_array_equal(narrow, expected.astype(np.uint16))
    assert wide.dtype == np.dtype(np.uint32)
    assert narrow.dtype == np.dtype(np.uint16)
