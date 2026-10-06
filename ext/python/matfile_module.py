"""``tttrlib.matfile`` -- MATLAB MAT-files, read and written like ``scipy.io``.

``loadmat``, ``savemat`` and ``whosmat`` take scipy.io's arguments and return
what scipy.io returns -- the same dicts, dtypes, shapes and squeeze rules -- over
tttrlib's C++ Level-5 reader/writer (``MatFile``), which carries its own
inflate/deflate. Level 5 is what MATLAB writes by default (``-v6``/``-v7``) and
what Octave writes with ``-mat``; ``-v7.3`` files are HDF5.

Not supported: sparse arrays, function handles and opaque objects. Such a
variable loads as the string ``"Read error: ..."`` with a warning, as scipy
does for a variable it cannot read; the rest of the file loads normally.
"""

import io as _io
import os
import time
import warnings

import numpy as np

import tttrlib as _t

__all__ = ["loadmat", "savemat", "whosmat", "MatStruct", "MatlabObject",
           "MatReadWarning", "MatWriteWarning"]


class MatReadWarning(UserWarning):
    """A variable in the file could not be read."""


class MatWriteWarning(UserWarning):
    """A value was skipped on writing (a name MATLAB cannot use)."""


class MatStruct:
    """One element of a MATLAB struct, for ``struct_as_record=False``.

    The fields are attributes; ``_fieldnames`` lists them in file order (the
    shape of scipy's ``mat_struct``).
    """

    def __init__(self):
        self._fieldnames = []

    def __repr__(self):
        return f"MatStruct({', '.join(self._fieldnames)})"


class MatlabObject(np.ndarray):
    """A MATLAB (pre-classdef) object: its struct array plus ``classname``."""

    def __new__(cls, input_array, classname=None):
        obj = np.asarray(input_array).view(cls)
        obj.classname = classname
        return obj

    def __array_finalize__(self, obj):
        self.classname = getattr(obj, "classname", None)


# --------------------------------------------------------------------- tables

_CELL, _STRUCT, _OBJECT, _CHAR, _SPARSE = 1, 2, 3, 4, 5
_FUNCTION, _OPAQUE, _UNREADABLE = 16, 17, -1

_CLASS_DTYPE = {6: "f8", 7: "f4", 8: "i1", 9: "u1", 10: "i2", 11: "u2",
                12: "i4", 13: "u4", 14: "i8", 15: "u8"}
_MI_DTYPE = {1: "i1", 2: "u1", 3: "i2", 4: "u2", 5: "i4", 6: "u4", 7: "f4",
             9: "f8", 12: "i8", 13: "u8", 16: "u1", 17: "u2", 18: "u4"}
_CLASS_NAME = {1: "cell", 2: "struct", 3: "object", 4: "char", 5: "sparse",
               6: "double", 7: "single", 8: "int8", 9: "uint8", 10: "int16",
               11: "uint16", 12: "int32", 13: "uint32", 14: "int64",
               15: "uint64", 16: "function", 17: "opaque"}
# numpy kind+itemsize -> (mxCLASS, miTYPE)
_NP_TO_MAT = {"f8": (6, 9), "f4": (7, 7), "i1": (8, 1), "u1": (9, 2),
              "i2": (10, 3), "u2": (11, 4), "i4": (12, 5), "u4": (13, 6),
              "i8": (14, 12), "u8": (15, 13), "b1": (9, 2)}
_MI_UTF8 = 16


def _np_code(dtype):
    return dtype.str[1:]


# ---------------------------------------------------------------------- read

def _squeeze_element(arr):
    if not arr.size:
        return np.empty((0,), dtype=arr.dtype)
    arr2 = np.squeeze(arr)
    # A 0-d result becomes a Python scalar (str, float, MatStruct, ...);
    # only a struct record stays an array, which is what scipy returns.
    if not arr2.shape and arr2.dtype.fields is None:
        return arr2.item()
    return arr2


def _chars_to_strings(arr):
    dims = list(arr.shape)
    last = dims.pop()
    if last == 0:
        return np.array([], dtype="<U1")
    arr = np.ascontiguousarray(arr)
    return np.ndarray(shape=tuple(dims), dtype=f"<U{last}", buffer=arr)


class _Reader:
    def __init__(self, mf, squeeze_me, struct_as_record, chars_as_strings, mat_dtype):
        self.mf = mf
        self.squeeze_me = squeeze_me
        self.struct_as_record = struct_as_record
        self.chars_as_strings = chars_as_strings
        self.mat_dtype = mat_dtype

    def _bytes(self, i, part="real"):
        return (self.mf.node_real(i) if part == "real" else self.mf.node_imag(i))

    def _numeric(self, i, dims):
        mf = self.mf
        real = np.frombuffer(self._bytes(i), dtype=_MI_DTYPE[mf.node_real_type(i)]).copy()
        res = real
        if mf.node_complex(i):
            imag = np.frombuffer(self._bytes(i, "imag"), dtype=_MI_DTYPE[mf.node_imag_type(i)]).copy()
            res = real + imag * 1j
        if mf.big_endian() and res.dtype.itemsize > 1 and not mf.node_complex(i):
            # scipy keeps a big-endian file's byte order in the dtype ('>f8');
            # the values are the same, the dtype is what a caller compares. (A
            # complex result is computed, real + 1j*imag, so it is native there too.)
            res = res.astype(res.dtype.newbyteorder(">"))
        return res.reshape(tuple(dims[::-1])).T

    def _char(self, i, dims):
        mf = self.mf
        mi = mf.node_real_type(i)
        raw = bytes(self._bytes(i))
        if mi in (16,):
            text = raw.decode("utf_8")
        elif mi in (17,):
            text = raw.decode("utf_16_le")
        elif mi in (18,):
            text = raw.decode("utf_32_le")
        elif mi == 4:  # miUINT16: UTF-16 code units
            text = raw.decode("utf_16_le")
        else:  # miINT8 / miUINT8
            text = raw.decode("latin1")
        n = int(np.prod(dims)) if len(dims) else 0
        if n == 0:
            return np.zeros(tuple(dims), dtype="U1")
        buf = np.array(text, dtype="U")
        return np.ndarray(shape=tuple(dims), dtype="U1", buffer=buf, order="F")

    def read(self, i, process=True):
        mf = self.mf
        mc = mf.node_class(i)
        dims = list(mf.node_dims(i))
        if mc == _UNREADABLE:
            raise _Unreadable(mf.node_error(i))
        if mc in _CLASS_DTYPE:
            arr = self._numeric(i, dims)
            if process and self.mat_dtype:
                if mf.node_logical(i):
                    arr = arr.astype(bool)
                else:
                    dt = np.dtype(_CLASS_DTYPE[mc])
                    if mf.big_endian() and dt.itemsize > 1:
                        dt = dt.newbyteorder(">")  # scipy's class dtypes follow the file
                    arr = arr.astype(dt)
        elif mc == _CHAR:
            arr = self._char(i, dims)
            if process and self.chars_as_strings:
                arr = _chars_to_strings(arr)
        elif mc == _CELL:
            children = mf.node_children(i)
            res = np.empty(len(children), dtype=object)
            for k, c in enumerate(children):
                res[k] = self.read(c)
            arr = res.reshape(tuple(dims[::-1])).T
        elif mc in (_STRUCT, _OBJECT):
            arr = self._struct(i, dims)
            if mc == _OBJECT:
                arr = MatlabObject(arr, mf.node_class_name(i))
        else:
            raise _Unreadable(f"unsupported MATLAB class {mc}")
        if process and self.squeeze_me:
            return _squeeze_element(arr)
        return arr

    def _struct(self, i, dims):
        mf = self.mf
        names = [mf.node_field_name(i, k) for k in range(mf.node_n_fields(i))]
        children = list(mf.node_children(i))
        tupdims = tuple(dims[::-1])
        length = int(np.prod(dims)) if dims else 0
        if self.struct_as_record:
            if not names:
                return np.empty(tupdims, dtype=object).T
            rec = np.empty(length, dtype=[(n, object) for n in names])
            it = iter(children)
            for k in range(length):
                for n in names:
                    rec[k][n] = self.read(next(it))
            return rec.reshape(tupdims).T
        res = np.empty(length, dtype=object)
        it = iter(children)
        for k in range(length):
            item = MatStruct()
            item._fieldnames = list(names)
            for n in names:
                item.__dict__[n] = self.read(next(it))
            res[k] = item
        return res.reshape(tupdims).T


class _Unreadable(Exception):
    pass


def _open_matfile(file_name, appendmat):
    mf = _t.MatFile()
    if hasattr(file_name, "read"):
        data = file_name.read()
        mf.read_buffer(np.frombuffer(data, dtype=np.uint8))
        return mf
    path = os.fspath(file_name)
    if appendmat and not os.path.exists(path) and not path.endswith(".mat"):
        path = path + ".mat"
    with open(path, "rb"):
        pass  # FileNotFoundError / PermissionError the way open() reports them
    mf.read(path)
    return mf


def _matstruct_to_dict(matobj):
    d = {}
    for name in matobj._fieldnames:
        elem = matobj.__dict__[name]
        if isinstance(elem, MatStruct):
            d[name] = _matstruct_to_dict(elem)
        elif _has_struct(elem):
            d[name] = _inspect_cell_array(elem)
        else:
            d[name] = elem
    return d


def _has_struct(elem):
    return (isinstance(elem, np.ndarray) and elem.size > 0 and elem.ndim > 0
            and isinstance(elem[0], MatStruct))


def _inspect_cell_array(ndarray):
    out = []
    for sub in ndarray:
        if isinstance(sub, MatStruct):
            out.append(_matstruct_to_dict(sub))
        elif _has_struct(sub):
            out.append(_inspect_cell_array(sub))
        else:
            out.append(sub)
    return out


def loadmat(file_name, mdict=None, appendmat=True, *, squeeze_me=False,
            struct_as_record=True, chars_as_strings=True, mat_dtype=False,
            simplify_cells=False, variable_names=None, **kwargs):
    """Load a MATLAB file, as :func:`scipy.io.loadmat` does.

    Parameters
    ----------
    file_name : str, os.PathLike or file-like
        The file (``.mat`` appended when missing and ``appendmat``), or an
        open binary file object.
    mdict : dict, optional
        Dictionary to insert the variables into.
    squeeze_me : bool
        Remove unit dimensions; a 1x1 array becomes a Python scalar.
    struct_as_record : bool
        Structs as numpy record arrays (True) or arrays of
        :class:`MatStruct` (False).
    chars_as_strings : bool
        Char arrays as arrays of strings (True) or of single characters.
    mat_dtype : bool
        Return MATLAB's class dtype rather than the storage dtype MATLAB
        compressed the data into.
    simplify_cells : bool
        Structs as nested dicts and cell arrays of structs as lists; implies
        ``squeeze_me=True`` and ``struct_as_record=False``.
    variable_names : None or sequence of str
        Read only these variables.

    Returns
    -------
    dict
        ``'__header__'``, ``'__version__'``, ``'__globals__'`` and one entry
        per variable.
    """
    if kwargs:
        unknown = sorted(set(kwargs) - {"verify_compressed_data_integrity",
                                        "uint16_codec", "byte_order", "spmatrix"})
        if unknown:
            raise TypeError(f"loadmat() got unexpected keyword arguments {unknown}")
    if simplify_cells:
        squeeze_me = True
        struct_as_record = False
    if isinstance(variable_names, str):
        variable_names = [variable_names]
    elif variable_names is not None:
        variable_names = list(variable_names)
    mf = _open_matfile(file_name, appendmat)
    reader = _Reader(mf, squeeze_me, struct_as_record, chars_as_strings, mat_dtype)
    out = {
        "__header__": mf.header_text().encode("latin1"),
        "__version__": f"{mf.version() >> 8}.{mf.version() & 0xff}",
        "__globals__": [],
    }
    for i in mf.top_level():
        name = mf.node_name(i)
        if name == "":
            name = "__function_workspace__"
        if variable_names is not None and name not in variable_names:
            continue
        if name in out:
            warnings.warn(f'Duplicate variable name "{name}" in stream - replacing previous with new',
                          MatReadWarning, stacklevel=2)
        try:
            out[name] = reader.read(i)
        except _Unreadable as err:
            warnings.warn(f'Unreadable variable "{name}", because "{err}"', MatReadWarning,
                          stacklevel=2)
            out[name] = f"Read error: {err}"
        if mf.node_class(i) != _UNREADABLE and mf.node_global(i):
            out["__globals__"].append(name)
    if simplify_cells:
        for key in list(out):
            if isinstance(out[key], MatStruct):
                out[key] = _matstruct_to_dict(out[key])
            elif _has_struct(out[key]):
                out[key] = _inspect_cell_array(out[key])
    if mdict is not None:
        mdict.update(out)
        return mdict
    return out


def whosmat(file_name, appendmat=True, **kwargs):
    """List the variables of a MATLAB file, as :func:`scipy.io.whosmat` does.

    Returns
    -------
    list of tuple
        ``(name, shape, class)`` per variable; a char array's shape drops its
        last (string-length) dimension.
    """
    mf = _open_matfile(file_name, appendmat)
    out = []
    for i in mf.top_level():
        mc = mf.node_class(i)
        if mc == _UNREADABLE:
            continue
        dims = tuple(int(d) for d in mf.node_dims(i))
        if mc == _CHAR:
            dims = dims[:-1]
        cls = "logical" if (mf.node_logical(i) and mc in _CLASS_DTYPE) else _CLASS_NAME.get(mc, "unknown")
        out.append((mf.node_name(i), dims, cls))
    return out


# --------------------------------------------------------------------- write

class _EmptyStruct:
    pass


def _to_writeable(source):
    if isinstance(source, np.ndarray):
        return source
    if source is None:
        return None
    if hasattr(source, "__array__"):
        return np.asarray(source)
    is_mapping = (hasattr(source, "keys") and hasattr(source, "values")
                  and hasattr(source, "items"))
    if isinstance(source, np.generic):
        pass
    elif not is_mapping and hasattr(source, "__dict__"):
        source = {k: v for k, v in source.__dict__.items() if not k.startswith("_")}
        is_mapping = True
    if is_mapping:
        dtype, values = [], []
        for field, value in source.items():
            if isinstance(field, str):
                if field[0] not in "_0123456789":
                    dtype.append((str(field), object))
                    values.append(value)
                else:
                    warnings.warn(f"Starting field name with a underscore or a digit ({field}) "
                                  "is ignored", MatWriteWarning, stacklevel=3)
        if dtype:
            return np.array([tuple(values)], dtype)
        return _EmptyStruct
    try:
        narr = np.asanyarray(source)
    except ValueError:
        narr = np.asanyarray(source, dtype=object)
    if narr.dtype.type in (object, np.object_) and narr.shape == () and narr == source:
        return None
    return narr


def _matdims(arr, oned_as):
    shape = arr.shape
    if shape == ():
        return (1, 1)
    if len(shape) == 1:
        if shape[0] == 0:
            return (0, 0)
        return (1,) + shape if oned_as == "row" else shape + (1,)
    return shape


class _Writer:
    def __init__(self, mf, long_field_names, oned_as):
        self.mf = mf
        self.long_field_names = long_field_names
        self.oned_as = oned_as

    def write(self, arr, name=""):
        mf = self.mf
        narr = _to_writeable(arr)
        if narr is None:
            raise TypeError(f"Could not convert {arr} (type {type(arr)}) to array")
        if narr is _EmptyStruct:
            return mf.add_node(name, _STRUCT, [1, 1])
        if isinstance(narr, MatlabObject):
            node = self._struct(narr, name, _OBJECT)
            mf.set_node_class_name(node, narr.classname or "")
            return node
        if narr.dtype.fields:
            return self._struct(narr, name, _STRUCT)
        if narr.dtype.hasobject:
            node = mf.add_node(name, _CELL, list(_matdims(narr, self.oned_as)))
            for el in np.atleast_2d(narr).flatten("F"):
                mf.add_child(node, self.write(el))
            return node
        if narr.dtype.kind in ("U", "S"):
            return self._char(narr, name)
        return self._numeric(narr, name)

    def _struct(self, arr, name, mclass):
        mf = self.mf
        fieldnames = list(arr.dtype.names)
        length = max(len(f) for f in fieldnames) + 1
        max_length = 64 if self.long_field_names else 32
        if length > max_length:
            raise ValueError(f"Field names are restricted to {max_length - 1} characters")
        node = mf.add_node(name, mclass, list(_matdims(arr, self.oned_as)))
        for f in fieldnames:
            mf.add_node_field_name(node, f)
        for el in np.atleast_2d(arr).flatten("F"):
            for f in fieldnames:
                mf.add_child(node, self.write(el[f]))
        return node

    def _char(self, arr, name):
        mf = self.mf
        if arr.size == 0 or np.all(arr == arr.dtype.type()):
            shape = (0,) * max(arr.ndim, 2)
            node = mf.add_node(name, _CHAR, list(shape), _MI_UTF8)
            return node
        if arr.dtype.kind == "S":
            arr = np.char.decode(arr, "latin1")
        # arr_to_chars: one more axis, the characters; short strings padded with blanks.
        dims = list(arr.shape) or [1]
        width = arr.dtype.itemsize // 4
        dims.append(width)
        chars = np.ndarray(shape=dims, dtype="U1", buffer=np.ascontiguousarray(arr))
        if np.any(chars == ""):
            chars = chars.copy()
            chars[chars == ""] = " "
        text = "".join(chars.flatten("F").tolist())
        data = np.frombuffer(text.encode("utf_8"), dtype=np.uint8)
        node = mf.add_node(name, _CHAR, list(chars.shape), _MI_UTF8)
        mf.set_node_real(node, data)
        return node

    def _numeric(self, arr, name):
        mf = self.mf
        imagf = arr.dtype.kind == "c"
        logif = arr.dtype.kind == "b"
        if imagf:
            part = arr.real
        else:
            part = arr
        code = _np_code(part.dtype.newbyteorder("=")) if part.dtype.byteorder not in ("=", "|") else _np_code(part.dtype)
        if code not in _NP_TO_MAT:
            if imagf:
                arr = arr.astype("c16")
            elif logif:
                arr = arr.astype("i1")
            else:
                arr = arr.astype("f8")
            part = arr.real if imagf else arr
            code = _np_code(part.dtype)
        mclass, mi = _NP_TO_MAT[code]
        if logif:
            mclass, mi = 9, 2
        node = mf.add_node(name, mclass, list(_matdims(arr, self.oned_as)), mi, imagf, logif, mi)

        def native(a):
            a = np.asarray(a)
            if a.dtype.byteorder not in ("=", "|"):
                a = a.astype(a.dtype.newbyteorder("="))
            return np.frombuffer(a.tobytes(order="F"), dtype=np.uint8)

        if imagf:
            mf.set_node_real(node, native(arr.real))
            mf.set_node_imag(node, native(arr.imag))
        else:
            mf.set_node_real(node, native(arr.astype(np.uint8) if logif else arr))
        return node


def savemat(file_name, mdict, appendmat=True, format="5", long_field_names=False,
            do_compression=False, oned_as="row"):
    """Save a dict of arrays as a MATLAB file, as :func:`scipy.io.savemat` does.

    Parameters
    ----------
    file_name : str, os.PathLike or file-like
        Destination (``.mat`` appended when missing and ``appendmat``), or an
        open binary file object.
    mdict : dict
        Variable name -> value. Names starting with ``_`` are skipped.
    format : {'5'}
        Only Level 5 is written.
    long_field_names : bool
        Allow struct field names up to 63 characters (31 otherwise).
    do_compression : bool
        Compress each variable (miCOMPRESSED).
    oned_as : {'row', 'column'}
        Whether 1-D arrays are written as row or column vectors.
    """
    if format != "5":
        raise ValueError("tttrlib.matfile writes Level-5 MAT-files only (format='5')")
    if oned_as not in ("row", "column"):
        raise ValueError("oned_as must be 'row' or 'column'")
    mf = _t.MatFile()
    mf.set_header_text(f"MATLAB 5.0 MAT-file Platform: {os.name}, Created on: {time.asctime()}")
    writer = _Writer(mf, long_field_names, oned_as)
    for name, var in mdict.items():
        if name == "__function_workspace__":
            name = ""
        elif name[0] == "_":
            warnings.warn(f"Starting field name with a underscore ({name}) is ignored",
                          MatWriteWarning, stacklevel=2)
            continue
        mf.add_top_level(writer.write(var, name))
    if hasattr(file_name, "write"):
        file_name.write(bytes(mf.save_buffer(bool(do_compression))))
        return
    path = os.fspath(file_name)
    if appendmat and not path.endswith(".mat"):
        path = path + ".mat"
    with open(path, "wb") as handle:
        handle.write(bytes(mf.save_buffer(bool(do_compression))))
