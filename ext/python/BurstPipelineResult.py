# SPDX-License-Identifier: BSD-3-Clause
#
# Pythonic surface for BurstPipelineResult, injected as methods by SWIG (%extend).
# Read path only, and no copies on it: every array is a view of a buffer the result
# owns (burstpipeline_support._bp_wrap keeps the result alive), except dense_decays,
# which says so.


def column_view(self, name):
    """Zero-copy view of the column *name* (float64 or int64).

    The view keeps this result alive. Columns of a run are listed by
    :meth:`column_names`; see the module README for what each one holds.

    Raises:
        KeyError: no column of that name.
    """
    if self.has_column(name):
        return _bp_wrap(self, self._column(name))
    if self.has_index_column(name):
        return _bp_wrap(self, self._index_column(name))
    raise KeyError(name)

def columns(self):
    """Every column as ``{name: view}``; no data is copied."""
    return {str(n): self.column_view(str(n)) for n in self.column_names()}

def decays(self, group):
    """CSR decays of decay group *group* (an index or a detector name).

    Returns:
        tuple: ``(indptr, bins, counts)`` zero-copy views. Row ``b`` (burst ``b``)
        is ``bins[indptr[b]:indptr[b + 1]]``, ``counts[...]`` the same slice; a bin
        index is ``role * n_bins + micro_bin`` with role 0 parallel, 1 perpendicular.
    """
    if not isinstance(group, int):
        g = self.decay_group_of(str(group))
        if g < 0:
            raise KeyError(group)
        group = g
    return (_bp_wrap(self, self._decay_indptr(group)),
            _bp_wrap(self, self._decay_bins(group)),
            _bp_wrap(self, self._decay_counts(group)))

def decay_sum(self, group):
    """Summed decay of every burst, ``2 * n_bins`` int64 (zero-copy)."""
    if not isinstance(group, int):
        group = self.decay_group_of(str(group))
    return _bp_wrap(self, self._decay_sum(group))

def dense_decays(self, group, rows=None):
    """The CSR decays of *group* as a dense ``(n_rows, 2 * n_bins)`` int64 matrix.

    This *copies*: it is for tests and small selections, not for the fit, which
    reads the CSR rows in place. *rows* selects bursts (default: all).
    """
    import numpy as np
    indptr, bins, counts = self.decays(group)
    g = group if isinstance(group, int) else self.decay_group_of(str(group))
    width = 2 * int(self.decay_n_bins(g))
    rows = np.arange(len(indptr) - 1) if rows is None else np.asarray(rows, dtype=np.int64)
    out = np.zeros((rows.size, width), dtype=np.int64)
    for k, b in enumerate(rows.tolist()):
        lo, hi = int(indptr[b]), int(indptr[b + 1])
        out[k, bins[lo:hi]] = counts[lo:hi]
    return out

def outside_histogram(self):
    """Raw micro-time histogram of the photons outside every burst.

    ``(len(histogram_channels()), histogram_length())`` int64 view, one row per
    routing channel of :meth:`histogram_channels`.
    """
    flat = _bp_wrap(self, self._outside_histogram())
    return flat.reshape(-1, max(1, int(self.histogram_length())))

def stats(self):
    """Timing, counts and sizes of the run (``stats_json`` parsed)."""
    import json
    return json.loads(self.stats_json())

def __len__(self):
    return int(self.n_bursts())
