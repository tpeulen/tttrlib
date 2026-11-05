# SPDX-License-Identifier: BSD-3-Clause
#
# Module-level support for BurstPipeline's Python surface: the owner-keeping views.
#
# At module scope, not inside %extend: SWIG injects a %pythoncode file into the
# class namespace, where a method cannot see a helper by its plain name.

import numpy as _np_bp


class _BpBuffer(object):
    """Exposes a raw ARGOUTVIEW array while holding the result object it points into.

    numpy's ARGOUTVIEW arrays carry no base object, so on their own nothing keeps
    the C++ buffer alive. ``numpy.asarray`` of this object returns a plain ndarray
    whose ``.base`` is this holder, and the holder keeps the owner -- so the view
    cannot outlive the buffer, however it is sliced or reshaped. An ndarray
    subclass would lose the owner when numpy collapses a view chain; an object that
    only exposes ``__array_interface__`` is not an ndarray, so the chain stops here
    (datastore_support._DsBuffer is the same construction).
    """

    __slots__ = ("_owner", "__array_interface__")

    def __init__(self, owner, flat):
        self._owner = owner
        self.__array_interface__ = flat.__array_interface__


def _bp_wrap(owner, flat):
    """Zero-copy view of *flat* (an ARGOUTVIEW array) that keeps *owner* alive."""
    flat = _np_bp.asarray(flat)
    if flat.size == 0:
        # an empty std::vector may hand out a null data pointer; numpy wants a real one
        return _np_bp.zeros(0, dtype=flat.dtype)
    return _np_bp.asarray(_BpBuffer(owner, flat))
