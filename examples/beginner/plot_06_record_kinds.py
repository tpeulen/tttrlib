"""
Basic Example 6: photons and scanner markers in one record stream
-----------------------------------------------------------------
A scanning (CLSM) measurement interleaves the scanner's marker records -- pixel,
line and frame clocks -- with the photons. This example

1. Loads a Becker & Hickl SPC-130 image scan (pixel clock, line clock, frame clock).
2. Asks ``tttrlib.CLSMImage`` what every record is: ``get_record_kinds`` names
   each record by the marker channels the file header and the reading routine imply.
3. Counts the records of each kind and draws the first line of the scan as a
   strip: one tick per record, coloured by kind.

The same classification works on raw arrays (``CLSMImage.classify_records_events``)
and on a TTTR with settings of your own (``CLSMImage.classify_records``).
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import tttrlib

# Make the `examples` package importable when this script is run directly,
# from any working directory.
sys.path[:0] = [str(_p) for _p in Path(__file__).resolve().parents
                if (_p / "examples" / "_example_data.py").is_file()][:1]

from examples._example_data import get_data_path

# %%
# Load the scan --------------------------------------------------------------
# The .set file beside the .spc tells tttrlib that the pixel clock was on.
filename = get_data_path("imaging/bh/spcm/FocalCheck_A1_20x_8xzoom_750nm_m1.spc")
tttr = tttrlib.TTTR(str(filename))

# %%
# Name every record ----------------------------------------------------------
# Building the image without pixels is enough: the image resolves the marker
# settings from the header, and get_record_kinds applies them to every record.
image = tttrlib.CLSMImage(tttr, build_pixels=False)
kinds = np.asarray(image.get_record_kinds())

names = {
    tttrlib.CLSM_RECORD_PHOTON: "photon",
    tttrlib.CLSM_RECORD_PIXEL_MARKER: "pixel",
    tttrlib.CLSM_RECORD_LINE_START_MARKER: "line start",
    tttrlib.CLSM_RECORD_LINE_STOP_MARKER: "line stop",
    tttrlib.CLSM_RECORD_FRAME_MARKER: "frame",
    tttrlib.CLSM_RECORD_OTHER_MARKER: "other marker",
}
counts = {names[code]: int(np.count_nonzero(kinds == code)) for code in names}
print(counts)

# %%
# Draw the first scan line ---------------------------------------------------
# Every record between the first two line-start markers, at its macro time.
line_starts = np.flatnonzero(kinds == tttrlib.CLSM_RECORD_LINE_START_MARKER)
first, last = line_starts[0], line_starts[1]
macro_ms = np.asarray(tttr.macro_times)[first:last + 1] * tttr.header.macro_time_resolution * 1e3
line_kinds = kinds[first:last + 1]

fig, (ax_strip, ax_bar) = plt.subplots(2, 1, figsize=(9, 5), gridspec_kw={"height_ratios": [1, 1.4]})
colours = {"photon": "0.6", "pixel": "tab:cyan", "line start": "tab:blue",
           "line stop": "tab:purple", "frame": "tab:orange", "other marker": "tab:red"}
for row, (code, name) in enumerate(names.items()):
    t = macro_ms[line_kinds == code]
    ax_strip.vlines(t, row, row + 0.8, color=colours[name], linewidth=0.6)
ax_strip.set_yticks(np.arange(len(names)) + 0.4, list(names.values()))
ax_strip.set_xlabel("macro time [ms]")
ax_strip.set_title("One scan line: photons between the pixel clock ticks")

ax_bar.bar(list(counts), list(counts.values()), color=[colours[n] for n in counts])
ax_bar.set_yscale("log")
ax_bar.set_ylabel("records")
ax_bar.set_title("Records of each kind in the file")
fig.tight_layout()
plt.show()
