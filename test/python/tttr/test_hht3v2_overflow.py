# SPDX-License-Identifier: BSD-3-Clause
"""HydraHarp V2 T3 overflow records: the count is in n_sync, and a count of 0
is one overflow (1024 syncs) -- PicoQuant's Read_PTU.py, the GENERIC_T3 reader
and the independent ptufile decoder all read it so. The HHT3v2 reader used to
add nothing for it, so every macro time after such a record was 1024 syncs
early. Found decoding a HydraHarp 400's FIFO words (phacq).
"""
import os
import struct
import tempfile
import unittest

import numpy as np

import tttrlib


def _tag(ident, typ, value):
    head = ident.encode().ljust(32, b"\0") + struct.pack("<iI", -1, typ)
    return head + (struct.pack("<d", value) if typ == 0x20000008 else struct.pack("<q", value))


def _write_ptu(path, words, rectype):
    h = b"PQTTTR\0\0" + b"1.0.00\0\0"
    h += _tag("Measurement_Mode", 0x10000008, 3)
    h += _tag("TTResultFormat_TTTRRecType", 0x10000008, rectype)
    h += _tag("TTResultFormat_BitsPerRecord", 0x10000008, 32)
    h += _tag("TTResult_NumberOfRecords", 0x10000008, len(words))
    h += _tag("MeasDesc_GlobalResolution", 0x20000008, 1e-7)
    h += _tag("MeasDesc_Resolution", 0x20000008, 1e-12)
    h += _tag("Header_End", 0xFFFF0008, 0)
    with open(path, "wb") as f:
        f.write(h + np.asarray(words, dtype="<u4").tobytes())


def _photon(channel, dtime, nsync):
    return (channel << 25) | (dtime << 10) | nsync


class TestHHT3v2Overflow(unittest.TestCase):

    def _macro_times(self, rectype):
        words = [_photon(1, 5, 10), 0xFE000000,       # overflow, count 0: one wrap
                 _photon(1, 5, 20), 0xFE000003,       # overflow, count 3
                 _photon(1, 5, 30)]
        with tempfile.TemporaryDirectory() as d:
            fn = os.path.join(d, "ofl.ptu")
            _write_ptu(fn, words, rectype)
            return np.asarray(tttrlib.TTTR(fn, "PTU").macro_times).tolist()

    def test_count_zero_is_one_overflow(self):
        # ptufile 2026.10.16 decodes these words to 10, 1044, 4126
        self.assertEqual(self._macro_times(0x01010304), [10, 1024 + 20, 4 * 1024 + 30])

    def test_hht3v2_matches_generic_t3(self):
        self.assertEqual(self._macro_times(0x01010304), self._macro_times(0x00010307))


if __name__ == "__main__":
    unittest.main()
