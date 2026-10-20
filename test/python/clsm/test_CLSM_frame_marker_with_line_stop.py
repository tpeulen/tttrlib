"""A frame marker that arrives with the last line stop of the frame.

PicoQuant marker records are bit fields. A scanner that raises its frame
signal together with the stop of a frame's last line therefore produces either
one record carrying both bits (frame 4 | line stop 2 = routing channel 6) or
the frame marker followed by the stop in the same few ticks. The Leica SP5 file
in the test data (SymPhoTime 64, TimeHarp 260P) has 210 of the first and 21 of
the second. Read from its header alone it reconstructed as one 1 x 7921 x 256
"salvaged" frame: the header names only the frame bit (4), so the merged
records went unseen, and each separate frame marker left the last line open and
put its stop at the head of the next frame, shifting every later (start, stop)
pair onto a fly-back.
"""

import os

import numpy as np
import pytest
import tttrlib

from test_settings import settings, DATA_AVAILABLE  # type: ignore

SP5 = settings.get("clsm_sp5_filename")


def stream_with_closing_frame_markers(merged):
    """Three frames of two lines x four pixels, two photons per pixel.

    Every frame is closed by a frame marker emitted with its last line stop:
    merged into one record (channel 6) or as a separate frame marker (4)
    followed by the stop (2) in the same tick.
    """
    macro, micro, channels, events = [], [], [], []

    def append(tick, channel, event, tac=0):
        macro.append(tick)
        micro.append(tac)
        channels.append(channel)
        events.append(event)

    append(0, 4, 1)  # the frame marker closing the frame before the recording
    tick = 10
    for frame in range(3):
        for line in range(2):
            append(tick, 1, 1)
            for pixel in range(4):
                append(tick + pixel, 0, 0, 3)
                append(tick + pixel, 0, 0, 5)
            stop = tick + 4
            last = line == 1
            if last and merged:
                append(stop, 6, 1)
            elif last:
                append(stop, 4, 1)
                append(stop, 2, 1)
            else:
                append(stop, 2, 1)
            tick += 10
    stream = tttrlib.TTTR(
        np.asarray(macro, dtype=np.uint64),
        np.asarray(micro, dtype=np.uint16),
        np.asarray(channels, dtype=np.int8),
        np.asarray(events, dtype=np.int8),
    )
    stream.header.set_macro_time_resolution(20e-6)
    stream.header.set_micro_time_resolution(1e-9)
    stream.header.set_number_of_micro_time_channels(64)
    return stream


@pytest.mark.parametrize("merged", [True, False])
def test_every_frame_keeps_its_last_line(merged):
    stream = stream_with_closing_frame_markers(merged)
    image = tttrlib.CLSMImage(
        tttr_data=stream,
        channels=[0],
        marker_frame_start=[4, 6],
        marker_line_start=1,
        marker_line_stop=2,
        marker_event_type=1,
        n_pixel_per_line=4,
        reading_routine="default",
        settings={"n_lines": 2},
        fill=True,
    )
    intensity = np.asarray(image.intensity)
    assert intensity.shape == (3, 2, 4)
    np.testing.assert_array_equal(intensity, np.full((3, 2, 4), 2))


@pytest.mark.skipif(
    not DATA_AVAILABLE or SP5 is None or not os.path.exists(SP5),
    reason="Leica SP5 test file not available",
)
def test_leica_sp5_reads_from_its_header_alone():
    tttr = tttrlib.TTTR(SP5)
    image = tttrlib.CLSMImage(tttr, channels=[0, 1, 2], fill=True)
    assert list(image.settings.marker_frame_start) == [4, 6]
    intensity = np.asarray(image.intensity)
    # 231 frame markers, 256 line starts between each pair; the 253 lines
    # before the first marker are a partial frame and are dropped
    assert intensity.shape == (230, 256, 256)
    # the last line is as bright as its neighbours, not a stub
    rows = intensity.sum(axis=(0, 2)).astype(float)
    assert rows[255] > 0.8 * rows[250:255].mean()
