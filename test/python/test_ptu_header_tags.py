"""PTU header tags read by tttrlib agree with the tag records in the file.

PicoQuant writes a true Bool8 tag as all ones in the 8-byte value; tttrlib once read
the first byte as a C++ bool (undefined for 0xff), so every enabled flag came out false,
and Int8 tags were cut to 32 bits. The reference here is a plain parse of the tag
records, independent of tttrlib.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

import pytest

import tttrlib
from test_settings import DATA_AVAILABLE, DATA_ROOT  # type: ignore

TY_BOOL8 = 0x00000008
TY_INT8 = 0x10000008
TY_ANSISTRING = 0x4001FFFF
TY_WIDESTRING = 0x4002FFFF
TY_BINARYBLOB = 0xFFFFFFFF
TY_FLOAT8ARRAY = 0x2001FFFF

FILES = ["pq/ptu/pq_ptu_hh_t3.ptu", "pq/ptu/pq_ptu_hh_t2.ptu"]


def reference_tags(path: Path) -> dict:
    """Parse the PTU tag records: ``{(name, idx): (type, raw uint64)}``."""
    tags = {}
    with open(path, "rb") as f:
        f.read(16)  # magic + version
        while True:
            ident, idx, typ, value = struct.unpack("<32siIQ", f.read(48))
            name = ident.split(b"\0", 1)[0].decode("latin-1")
            if name == "Header_End":
                return tags
            tags[(name, idx)] = (typ, value)
            if typ in (TY_ANSISTRING, TY_WIDESTRING, TY_BINARYBLOB, TY_FLOAT8ARRAY):
                f.read(value)


def tttrlib_tags(path: Path) -> dict:
    """The header tags tttrlib reports: ``{(name, idx): value}``."""
    header = json.loads(tttrlib.TTTR(str(path)).header.json)
    return {(t["name"], t["idx"]): t["value"] for t in header["tags"]}


@pytest.mark.skipif(not DATA_AVAILABLE, reason="test data not available")
@pytest.mark.parametrize("rel", FILES)
def test_bool8_and_int8_tags_match_the_file(rel):
    path = Path(DATA_ROOT) / rel
    if not path.exists():
        pytest.skip(f"{rel} not in the test data")
    ref, got = reference_tags(path), tttrlib_tags(path)
    checked = {"bool": 0, "int": 0, "true": 0}
    for key, (typ, raw) in ref.items():
        if typ == TY_BOOL8:
            assert got[key] is (raw != 0), f"{key}: file {raw:#x}, tttrlib {got[key]!r}"
            checked["bool"] += 1
            checked["true"] += raw != 0
        elif typ == TY_INT8:
            assert got[key] == struct.unpack("<q", struct.pack("<Q", raw))[0], key
            checked["int"] += 1
    # The files carry both kinds, including true flags -- otherwise the test proves nothing.
    assert checked["bool"] and checked["int"] and checked["true"], checked
