# SPDX-License-Identifier: BSD-3-Clause
"""A PTU with only the essential tags reads in full.

Every PicoQuant PTU record type is 32 bit, so a file without
``TTResultFormat_BitsPerRecord`` (hand-made, third-party writers) must not read
as zero records, which it silently did. ``MeasDesc_BinningFactor`` is optional
(no binning) and its absence is not an error. Found writing HydraHarp 400 FIFO
words to PTU for a decoder cross-check (phacq).
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

import numpy as np

import tttrlib


def _tag(ident, typ, value):
    head = ident.encode().ljust(32, b"\0") + struct.pack("<iI", -1, typ)
    return head + (struct.pack("<d", value) if typ == 0x20000008 else struct.pack("<q", value))


def _write_minimal_ptu(path, words):
    h = b"PQTTTR\0\0" + b"1.0.00\0\0"
    h += _tag("TTResultFormat_TTTRRecType", 0x10000008, 0x01010204)   # HydraHarp V2 T2
    h += _tag("TTResult_NumberOfRecords", 0x10000008, len(words))
    h += _tag("MeasDesc_GlobalResolution", 0x20000008, 1e-12)
    h += _tag("MeasDesc_Resolution", 0x20000008, 1e-12)
    h += _tag("Header_End", 0xFFFF0008, 0)
    with open(path, "wb") as f:
        f.write(h + np.asarray(words, dtype="<u4").tobytes())


WORDS = [0x80000000 | 100, (1 << 25) | 150, 0x80000000 | 200, (2 << 25) | 260]  # sync, in1, sync, in2


class TestPtuMinimalHeader(unittest.TestCase):

    def test_records_read_without_bits_per_record(self):
        with tempfile.TemporaryDirectory() as d:
            fn = os.path.join(d, "minimal.ptu")
            _write_minimal_ptu(fn, WORDS)
            t = tttrlib.TTTR(fn)
            self.assertEqual(len(t), 4)
            self.assertEqual(np.asarray(t.macro_times).tolist(), [100, 150, 200, 260])
            self.assertEqual(np.asarray(t.routing_channels).tolist(), [0, 2, 0, 3])

    def test_missing_binning_factor_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as d:
            fn = os.path.join(d, "minimal.ptu")
            _write_minimal_ptu(fn, WORDS)
            code = f"import tttrlib; print(len(tttrlib.TTTR({fn!r})))"
            flags = ["-S"] if sys.flags.no_site else []   # same import setup as this run
            run = subprocess.run([sys.executable, *flags, "-c", code], capture_output=True,
                                 text=True, env=os.environ.copy())
            self.assertEqual(run.stdout.strip().splitlines()[-1], "4")
            self.assertNotIn("ERROR", run.stderr)


if __name__ == "__main__":
    unittest.main()
