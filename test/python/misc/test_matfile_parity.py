"""A/B of `tttrlib.matfile` (MatFile.h + its own inflate/deflate) against scipy.io.

`loadmat`/`savemat`/`whosmat` exist so that callers can drop scipy.io without
anything they read moving, so the claim pinned here is *identity*: the same
dict, the same types, the same dtypes and shapes, the same values, under every
loader option scipy offers the callers use. Inputs are a real MATLAB (PCWIN,
compressed) file, a real Octave session (nested structs, UTF-16 chars), files
scipy writes for every class and option, and hand-built files for the corners
scipy never writes (big-endian, zero-byte cell elements). The writer is pinned
from the other side: what it writes, scipy reads back exactly as it reads its
own output -- and uncompressed, the bytes after the header are scipy's bytes.

scipy is the oracle and is optional: without it the whole file skips.
"""

import io
import pathlib
import struct
import warnings
import zlib

import numpy as np
import pytest

sio = pytest.importorskip("scipy.io")
sparse = pytest.importorskip("scipy.sparse")
tm = pytest.importorskip("tttrlib.matfile")
from scipy.io.matlab import mat_struct, MatlabObject  # noqa: E402

DATA = pathlib.Path(__file__).resolve().parents[2] / "data" / "matfile"
FIXTURES = sorted(DATA.glob("*.mat"))
RNG = np.random.default_rng(20261006)

OPTION_SETS = [
    {},
    {"squeeze_me": True},
    {"struct_as_record": False},
    {"squeeze_me": True, "struct_as_record": False},
    {"chars_as_strings": False},
    {"mat_dtype": True},
    {"squeeze_me": True, "mat_dtype": True, "chars_as_strings": False},
    {"simplify_cells": True},
]


def same(ours, ref, path="root"):
    """Assert two loadmat results are identical in type, dtype, shape and value."""
    if isinstance(ref, mat_struct):
        assert isinstance(ours, tm.MatStruct), (path, type(ours))
        assert list(ours._fieldnames) == list(ref._fieldnames), path
        for name in ref._fieldnames:
            same(getattr(ours, name), getattr(ref, name), f"{path}.{name}")
        return
    if isinstance(ref, dict):
        assert isinstance(ours, dict), (path, type(ours))
        assert list(ours) == list(ref), (path, list(ours), list(ref))
        for k in ref:
            same(ours[k], ref[k], f"{path}[{k!r}]")
        return
    if isinstance(ref, list):
        assert isinstance(ours, list) and len(ours) == len(ref), path
        for k, (a, b) in enumerate(zip(ours, ref)):
            same(a, b, f"{path}[{k}]")
        return
    if isinstance(ref, np.ndarray):
        assert isinstance(ours, np.ndarray), (path, type(ours))
        if isinstance(ref, MatlabObject):
            assert isinstance(ours, tm.MatlabObject) and ours.classname == ref.classname, path
        assert ours.shape == ref.shape, (path, ours.shape, ref.shape)
        assert ours.dtype == ref.dtype, (path, ours.dtype, ref.dtype)
        if ref.dtype.names is not None:
            for idx in np.ndindex(ref.shape):
                for name in ref.dtype.names:
                    same(ours[idx][name], ref[idx][name], f"{path}{list(idx)}.{name}")
        elif ref.dtype == object:
            for idx in np.ndindex(ref.shape):
                same(ours[idx], ref[idx], f"{path}{list(idx)}")
        else:
            assert np.array_equal(ours, ref, equal_nan=ref.dtype.kind in "fc"), path
        return
    assert type(ours) is type(ref), (path, type(ours), type(ref))
    if isinstance(ref, float) and np.isnan(ref):
        assert np.isnan(ours), path
    else:
        assert ours == ref, (path, ours, ref)


def load_both(source, **opts):
    if isinstance(source, (bytes, bytearray)):
        ours = tm.loadmat(io.BytesIO(source), **opts)
        ref = sio.loadmat(io.BytesIO(source), **opts)
    else:
        ours = tm.loadmat(str(source), **opts)
        ref = sio.loadmat(str(source), **opts)
    return ours, ref


def scipy_bytes(mdict, **kw):
    buf = io.BytesIO()
    sio.savemat(buf, mdict, **kw)
    return buf.getvalue()


def ours_bytes(mdict, **kw):
    buf = io.BytesIO()
    tm.savemat(buf, mdict, **kw)
    return buf.getvalue()


# A dict exercising every class and shape rule the writers share.
def zoo():
    RNG = np.random.default_rng(7)  # same values for every writer compared
    return {
        "dbl": RNG.normal(size=(3, 4)),
        "row": np.arange(5.0),
        "empty": np.zeros((0, 0)),
        "empty_row": np.zeros((0,)),
        "scalar": 2.5,
        "integer": 7,
        "flag": True,
        "logic": np.array([[True, False], [False, True]]),
        "f32": RNG.normal(size=(2, 3)).astype(np.float32),
        "i8": np.array([-3, 0, 5], dtype=np.int8),
        "u8": np.arange(6, dtype=np.uint8).reshape(2, 3),
        "i16": np.array([-300, 2], dtype=np.int16),
        "u16": np.array([60000], dtype=np.uint16),
        "i32": np.arange(-2, 7, dtype=np.int32).reshape(3, 3),
        "u32": np.array([4_000_000_000], dtype=np.uint32),
        "i64": np.array([-(2**40), 2**40], dtype=np.int64),
        "u64": np.array([2**63 + 5], dtype=np.uint64),
        "cplx": RNG.normal(size=(2, 2)) + 1j * RNG.normal(size=(2, 2)),
        "cplx64": (np.arange(3) + 1j).astype(np.complex64),
        "nd": RNG.normal(size=(2, 3, 4)),
        "text": "hello world",
        "unicode": "Förster ΔG µs",
        "empty_text": "",
        "texts": np.array(["ab", "c", "def"]),
        "char_matrix": np.array([["ab", "cd"], ["ef", "gh"]]),
        "cell": np.array([1.0, "two", np.arange(3)], dtype=object),
        "cell2d": np.array([[np.zeros((2, 2)), "x"], [np.ones(1), np.array([], dtype=object)]],
                           dtype=object),
        "struct": {"a": 1.0, "b": "text", "c": np.arange(4), "nested": {"deep": np.eye(2)}},
        "empty_struct": {},
        "struct_array": np.array(
            [[(1.0, "x"), (2.0, "yy")]], dtype=[("v", object), ("s", object)]),
        "struct_in_cell": np.array([{"q": 1.0}, {"q": 2.0}], dtype=object),
    }


@pytest.mark.parametrize("fixture", FIXTURES, ids=lambda p: p.name)
@pytest.mark.parametrize("opts", OPTION_SETS, ids=str)
def test_real_files_load_like_scipy(fixture, opts):
    ours, ref = load_both(fixture, **opts)
    ref.pop("__header__")
    assert ours.pop("__header__")
    same(ours, ref)


@pytest.mark.parametrize("compress", [False, True])
@pytest.mark.parametrize("oned_as", ["row", "column"])
@pytest.mark.parametrize("opts", OPTION_SETS, ids=str)
def test_scipy_written_files_load_like_scipy(compress, oned_as, opts):
    data = scipy_bytes(zoo(), do_compression=compress, oned_as=oned_as)
    ours, ref = load_both(data, **opts)
    assert ours["__header__"] == ref["__header__"]
    same(ours, ref)


@pytest.mark.parametrize("oned_as", ["row", "column"])
def test_uncompressed_bytes_are_scipys(oned_as):
    a = scipy_bytes(zoo(), oned_as=oned_as)
    b = ours_bytes(zoo(), oned_as=oned_as)
    assert a[116:] == b[116:]  # everything past the free-text header


@pytest.mark.parametrize("compress", [False, True])
@pytest.mark.parametrize("opts", OPTION_SETS, ids=str)
def test_our_files_load_in_scipy_like_scipys_own(compress, opts):
    mine = sio.loadmat(io.BytesIO(ours_bytes(zoo(), do_compression=compress)), **opts)
    theirs = sio.loadmat(io.BytesIO(scipy_bytes(zoo(), do_compression=compress)), **opts)
    mine.pop("__header__")
    theirs.pop("__header__")
    same_scipy(mine, theirs)


def same_scipy(a, b):
    """`same` for two scipy results (both sides use scipy's own classes)."""
    if isinstance(b, mat_struct):
        assert isinstance(a, mat_struct) and list(a._fieldnames) == list(b._fieldnames)
        for n in b._fieldnames:
            same_scipy(getattr(a, n), getattr(b, n))
    elif isinstance(b, dict):
        assert list(a) == list(b)
        for k in b:
            same_scipy(a[k], b[k])
    elif isinstance(b, list):
        assert len(a) == len(b)
        for x, y in zip(a, b):
            same_scipy(x, y)
    elif isinstance(b, np.ndarray):
        assert a.shape == b.shape and a.dtype == b.dtype
        if b.dtype.names is not None:
            for idx in np.ndindex(b.shape):
                for n in b.dtype.names:
                    same_scipy(a[idx][n], b[idx][n])
        elif b.dtype == object:
            for idx in np.ndindex(b.shape):
                same_scipy(a[idx], b[idx])
        else:
            assert np.array_equal(a, b, equal_nan=b.dtype.kind in "fc")
    else:
        assert type(a) is type(b) and a == b


def test_long_field_names_rule():
    long = {"s": {"x" * 40: 1.0}}
    with pytest.raises(ValueError):
        tm.savemat(io.BytesIO(), long)
    data = ours_bytes(long, long_field_names=True)
    same_scipy(sio.loadmat(io.BytesIO(data))["s"]["x" * 40][0, 0], np.array([[1.0]]))


def test_underscore_names_are_skipped_with_a_warning():
    with pytest.warns(tm.MatWriteWarning):
        data = ours_bytes({"_hidden": 1.0, "shown": 2.0})
    assert [k for k in sio.loadmat(io.BytesIO(data)) if not k.startswith("__")] == ["shown"]


def test_variable_names_and_whosmat():
    data = scipy_bytes(zoo())
    ours, ref = (tm.loadmat(io.BytesIO(data), variable_names=["dbl", "text"]),
                 sio.loadmat(io.BytesIO(data), variable_names=["dbl", "text"]))
    ours.pop("__header__"), ref.pop("__header__")
    same(ours, ref)
    assert tm.whosmat(io.BytesIO(data)) == sio.whosmat(io.BytesIO(data))
    for fixture in FIXTURES:
        assert tm.whosmat(str(fixture)) == sio.whosmat(str(fixture))


def test_sparse_variable_is_reported_unreadable_not_fatal():
    data = scipy_bytes({"before": 1.0, "sp": sparse.csc_matrix(np.eye(3)), "after": 2.0})
    with pytest.warns(tm.MatReadWarning, match="sp"):
        ours = tm.loadmat(io.BytesIO(data))
    assert ours["sp"].startswith("Read error")
    assert ours["before"][0, 0] == 1.0 and ours["after"][0, 0] == 2.0


def test_appendmat_and_paths(tmp_path):
    tm.savemat(str(tmp_path / "x"), {"a": np.arange(3.0)})
    assert (tmp_path / "x.mat").exists()
    same(tm.loadmat(str(tmp_path / "x"))["a"], sio.loadmat(str(tmp_path / "x.mat"))["a"])


# ------------------------------------------------- hand-built corner cases

def _el(mtype, payload, big=False):
    fmt = ">II" if big else "<II"
    out = struct.pack(fmt, mtype, len(payload)) + payload
    return out + b"\0" * (-len(out) % 8)


def _matrix(name, mclass, dims, body, big=False, flags=0):
    e = ">" if big else "<"
    af = _el(6, struct.pack(e + "II", mclass | (flags << 8), 0), big)
    dm = _el(5, struct.pack(e + "%di" % len(dims), *dims), big)
    nm = _el(1, name.encode(), big)
    return _el(14, af + dm + nm + body, big)


def _file(variables, big=False):
    head = b"MATLAB 5.0 MAT-file, hand built".ljust(116, b" ") + b"\0" * 8
    head += (b"\x01\x00MI" if big else b"\x00\x01IM")
    return head + b"".join(variables)


@pytest.mark.parametrize("opts", OPTION_SETS[:6], ids=str)
def test_big_endian_file(opts):
    data = np.arange(6.0).reshape(2, 3)
    dbl = _matrix("x", 6, [2, 3], _el(9, data.T.astype(">f8").tobytes(), True), big=True)
    chars = _matrix("s", 4, [1, 3], _el(4, np.array([104, 105, 33], ">u2").tobytes(), True), big=True)
    ints = _matrix("k", 12, [1, 2], _el(5, np.array([-7, 9], ">i4").tobytes(), True), big=True)
    blob = _file([dbl, chars, ints], big=True)
    ours, ref = load_both(blob, **opts)
    same(ours, ref)


def test_zero_byte_cell_element_and_small_elements():
    # MATLAB writes an empty cell element as a zero-byte miMATRIX.
    empty = struct.pack("<II", 14, 0)
    one = _matrix("", 6, [1, 1], _el(9, struct.pack("<d", 4.5)))
    # A small-data element: uint8 3 stored in the tag (MATLAB compresses
    # doubles into the smallest integer type that holds them).
    small = _matrix("", 6, [1, 1], struct.pack("<I", (1 << 16) | 2) + bytes([3, 0, 0, 0]))
    cell = _matrix("c", 1, [1, 3], empty + one + small)
    blob = _file([cell])
    for opts in OPTION_SETS:
        ours, ref = load_both(blob, **opts)
        same(ours, ref)


def test_compressed_hand_built_variable_with_dynamic_huffman():
    payload = _matrix("z", 6, [1, 4096], _el(9, np.tile(np.arange(16.0), 256).tobytes()))
    comp = zlib.compress(payload, 9)  # dynamic Huffman blocks
    blob = _file([struct.pack("<II", 15, len(comp)) + comp])
    ours, ref = load_both(blob)
    same(ours, ref)
    # and a stored (uncompressed) deflate block
    stored = zlib.compressobj(0)
    comp0 = stored.compress(payload) + stored.flush()
    ours, ref = load_both(_file([struct.pack("<II", 15, len(comp0)) + comp0]))
    same(ours, ref)


def test_corrupt_compressed_data_raises():
    payload = _matrix("z", 6, [1, 4], _el(9, np.arange(4.0).tobytes()))
    comp = bytearray(zlib.compress(payload))
    comp[-1] ^= 0xFF  # break the Adler-32
    blob = _file([struct.pack("<II", 15, len(comp)) + bytes(comp)])
    with pytest.raises(Exception):
        tm.loadmat(io.BytesIO(blob))


def test_deflate_round_trip_through_zlib():
    for data in [b"", b"a", bytes(range(256)) * 50, RNG.integers(0, 4, 100_000).astype(np.uint8).tobytes(),
                 np.arange(10_000.0).tobytes()]:
        out = ours_bytes({"b": np.frombuffer(data, dtype=np.uint8)}, do_compression=True)
        back = sio.loadmat(io.BytesIO(out))["b"]
        assert back.tobytes() == data or (data == b"" and back.size == 0)
    # the compressed stream is a valid zlib stream scipy/zlib inflate
    raw = ours_bytes({"x": np.zeros(50_000)}, do_compression=True)
    n = struct.unpack("<I", raw[132:136])[0]
    assert len(zlib.decompress(raw[136:136 + n])) > 400_000
    assert n < 4_000  # 400 kB of zeros: fixed-Huffman LZ77 lands near 2.6 kB


def test_warnings_free_on_real_files():
    for fixture in FIXTURES:
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            tm.loadmat(str(fixture))
