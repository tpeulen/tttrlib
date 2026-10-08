"""Deferred channel splitting preserves the same layout as constructor filling."""

import numpy as np
import pytest
import tttrlib


def scanner_stream():
    """Build two frames with distinct routing-channel counts and exact photon indices."""
    macro, micro, channels, events = [], [], [], []
    expected = np.empty((2, 2, 2, 2), dtype=np.uint32)
    indices = {}

    def append(tick, channel, event, count=1):
        start = len(macro)
        macro.extend([tick] * count)
        micro.extend([11] * count)
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
                for channel, routing in enumerate((0, 3)):
                    count = 2 + frame * 8 + line * 4 + pixel * 2 + channel
                    expected[channel, frame, line, pixel] = count
                    indices[channel, frame, line, pixel] = append(
                        tick + pixel * 2 + 1, routing, 0, count
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
    return stream, expected, indices


def image_for(stream, *, split_by_channel, build_pixels, fill):
    """Construct an image with explicit scan geometry and optional constructor fill."""
    return tttrlib.CLSMImage(
        tttr_data=stream,
        channels=[0, 3],
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
        fill=fill,
    )


def assert_layout_and_photon_indices(image, expected, indices, *, split_by_channel):
    """Verify public layout metadata and every pixel's original photon identities."""
    wanted = expected if split_by_channel else expected.sum(axis=0)
    assert image.n_channels == (2 if split_by_channel else 1)
    assert tuple(image.shape) == wanted.shape
    assert image.n_frames == (4 if split_by_channel else 2)
    flat = expected.reshape(4, 2, 2) if split_by_channel else wanted
    np.testing.assert_array_equal(image.get_intensity_u32(), flat)
    if split_by_channel:
        assert [image.get_channel_frame_count(c) for c in range(2)] == [2, 2]
        assert [image.get_channel_frame_offset(c) for c in range(2)] == [0, 2]
        assert [image.get_channel_of_frame(f) for f in range(4)] == [0, 0, 1, 1]
        assert tuple(image[0].shape) == tuple(image[1].shape) == (2, 2, 2)

    actual_indices = []
    for channel, frame, line, pixel in indices:
        if split_by_channel:
            actual = image[channel][frame][line][pixel].tttr_indices
            wanted_indices = indices[channel, frame, line, pixel]
        else:
            if channel != 0:
                continue
            actual = image[frame][line][pixel].tttr_indices
            wanted_indices = np.sort(np.concatenate([
                indices[0, frame, line, pixel], indices[1, frame, line, pixel],
            ]))
        np.testing.assert_array_equal(actual, wanted_indices)
        actual_indices.extend(actual)
    assert len(actual_indices) == len(set(actual_indices)) == int(expected.sum())
    np.testing.assert_array_equal(image.get_intensity_u32(), flat)


@pytest.mark.parametrize("build_pixels", [False, True])
@pytest.mark.parametrize("split_by_channel", [False, True])
def test_deferred_fill_matches_constructor_layout_and_photon_indices(
    build_pixels, split_by_channel
):
    """An explicit fill exposes the same channel blocks as filling during construction."""
    stream, expected, indices = scanner_stream()
    reference = image_for(
        stream, split_by_channel=split_by_channel, build_pixels=build_pixels, fill=True
    )
    image = image_for(
        stream, split_by_channel=split_by_channel, build_pixels=build_pixels, fill=False
    )
    image.fill(stream, channels=[0, 3])
    assert tuple(image.shape) == tuple(reference.shape)
    assert image.n_channels == reference.n_channels
    assert_layout_and_photon_indices(
        image, expected, indices, split_by_channel=split_by_channel
    )
    assert_layout_and_photon_indices(
        reference, expected, indices, split_by_channel=split_by_channel
    )


@pytest.mark.parametrize("build_pixels", [False, True])
@pytest.mark.parametrize("materialize", [False, True])
def test_repeated_deferred_fill_reuses_channel_blocks(build_pixels, materialize):
    """Cleared refills keep two channels and four frames without duplicating frame blocks."""
    stream, expected, indices = scanner_stream()
    image = image_for(stream, split_by_channel=True, build_pixels=build_pixels, fill=False)
    image.fill(stream, channels=[0, 3])
    if materialize:
        image.frame_at(0)
    image.fill(stream, channels=[0, 3], clear=True)
    image.fill(stream, channels=[0, 3], clear=True)
    assert_layout_and_photon_indices(image, expected, indices, split_by_channel=True)


@pytest.mark.parametrize("build_pixels", [False, True])
def test_explicit_split_updates_layout_before_fill(build_pixels):
    """The public split operation immediately declares the new empty channel layout."""
    stream, expected, indices = scanner_stream()
    image = image_for(stream, split_by_channel=False, build_pixels=build_pixels, fill=False)
    assert image.split_frames_by_channel([0, 3], stream) == 2
    assert image.n_channels == 2
    assert tuple(image.shape) == (2, 2, 2, 2)
    assert image.n_frames == 4
    np.testing.assert_array_equal(image.get_intensity_u32(), np.zeros((4, 2, 2)))
    image.fill(stream, channels=[0, 3])
    assert_layout_and_photon_indices(image, expected, indices, split_by_channel=True)


@pytest.mark.parametrize("constructor_fill", [False, True])
def test_accumulating_refill_keeps_blocks_and_existing_duplicate_photon_semantics(
    constructor_fill,
):
    """An uncleared refill doubles photon counts without increasing the number of frames."""
    stream, expected, indices = scanner_stream()
    image = image_for(
        stream, split_by_channel=True, build_pixels=False, fill=constructor_fill
    )
    if not constructor_fill:
        image.fill(stream, channels=[0, 3])
    image.fill(stream, channels=[0, 3], clear=False)
    assert image.n_channels == 2
    assert tuple(image.shape) == (2, 2, 2, 2)
    assert image.n_frames == 4
    np.testing.assert_array_equal(image.get_intensity_u32(), 2 * expected.reshape(4, 2, 2))
    for (channel, frame, line, pixel), wanted in indices.items():
        actual = image[channel][frame][line][pixel].tttr_indices
        np.testing.assert_array_equal(np.sort(actual), np.sort(np.tile(wanted, 2)))
