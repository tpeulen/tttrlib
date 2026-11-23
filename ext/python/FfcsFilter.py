# SPDX-License-Identifier: BSD-3-Clause
# Filtered FCS (fFCS): species filters as a tttrlib object and file format.
#
# A filter assigns every photon one weight per species from its routing channel
# and micro time; correlating the weighted streams gives species auto- and
# cross-correlations (Felekyan, Kalinin, Sanabria, Valeri, Seidel, ChemPhysChem
# 2012). ``FfcsFilter`` holds the filters, computes the weights, reads the
# legacy SoftCorr/FLF ``.jef`` files and stores filters as ``.ffcs.json``
# (format ``tttrlib.ffcs_filter``, version 1).

import json as _ffcs_json

import numpy as _ffcs_np


class FfcsFilter(object):
    """Species filters of filtered FCS.

    Parameters
    ----------
    species : list of str
        Species names (rows of every filter).
    filters : dict
        ``{routing_channel: array (n_species, n_bins)}``, the filter of each
        detection channel over binned micro time.
    micro_time_binning : int
        Micro-time channels (of the instrument file) per filter bin.
    patterns : dict, optional
        ``{routing_channel: array (n_species, n_bins)}``, the species decay
        patterns the filters were made from (normalized jointly over all
        channels), kept for reference.
    metadata : dict, optional
        Free-form provenance (source file, sha256, ...).
    """

    FORMAT = "tttrlib.ffcs_filter"
    VERSION = 1

    def __init__(self, species, filters, micro_time_binning=1, patterns=None, metadata=None):
        self.species = [str(s) for s in species]
        self.filters = {int(k): _ffcs_np.asarray(v, dtype=_ffcs_np.float64) for k, v in filters.items()}
        n = len(self.species)
        for ch, f in self.filters.items():
            if f.ndim != 2 or f.shape[0] != n:
                raise ValueError("filter of channel %d: expected shape (%d, n_bins), got %s" % (ch, n, f.shape))
        self.micro_time_binning = int(micro_time_binning)
        if self.micro_time_binning < 1:
            raise ValueError("micro_time_binning must be >= 1")
        self.patterns = None if patterns is None else {
            int(k): _ffcs_np.asarray(v, dtype=_ffcs_np.float64) for k, v in patterns.items()}
        self.metadata = dict(metadata or {})

    @property
    def channels(self):
        return sorted(self.filters)

    @property
    def n_species(self):
        return len(self.species)

    def weights(self, micro_times, routing_channels):
        """Per-photon weights, array ``(n_species, n_photons)``. Photons of a
        channel without filter, or beyond its last bin, get weight 0."""
        mt = _ffcs_np.asarray(micro_times, dtype=_ffcs_np.int64)
        ch = _ffcs_np.asarray(routing_channels, dtype=_ffcs_np.int64)
        if mt.shape != ch.shape:
            raise ValueError("micro_times and routing_channels differ in length")
        w = _ffcs_np.zeros((self.n_species, mt.size), dtype=_ffcs_np.float64)
        b = mt // self.micro_time_binning
        for c, f in self.filters.items():
            sel = (ch == c) & (b < f.shape[1]) & (b >= 0)
            w[:, sel] = f[:, b[sel]]
        return w

    def identity_check(self):
        """``F @ P`` over all channels (identity for consistent filters and
        patterns); None without patterns."""
        if not self.patterns:
            return None
        return sum(self.filters[c] @ self.patterns[c].T for c in self.channels)

    # ------------------------------------------------------------- file format
    def to_dict(self):
        d = {"format": self.FORMAT, "version": self.VERSION, "species": self.species,
             "micro_time_binning": self.micro_time_binning,
             "filters": {str(c): self.filters[c].tolist() for c in self.channels},
             "metadata": self.metadata}
        if self.patterns:
            d["patterns"] = {str(c): self.patterns[c].tolist() for c in sorted(self.patterns)}
        return d

    @classmethod
    def from_dict(cls, d):
        if d.get("format") != cls.FORMAT:
            raise ValueError("not a %s document" % cls.FORMAT)
        if int(d.get("version", 0)) > cls.VERSION:
            raise ValueError("ffcs_filter version %s is newer than this tttrlib" % d.get("version"))
        return cls(d["species"], {int(k): v for k, v in d["filters"].items()},
                   d.get("micro_time_binning", 1),
                   patterns={int(k): v for k, v in d["patterns"].items()} if d.get("patterns") else None,
                   metadata=d.get("metadata"))

    def save(self, path):
        with open(path, "w") as fh:
            _ffcs_json.dump(self.to_dict(), fh)

    @classmethod
    def load(cls, path):
        with open(path) as fh:
            return cls.from_dict(_ffcs_json.load(fh))

    # ----------------------------------------------------------- legacy input
    @classmethod
    def from_legacy_jef(cls, path, channel_groups=((1, 0, 3, 2), (5, 4, 7, 6)), micro_time_binning=32,
                        species=None, tac_dat=None):
        """Read a SoftCorr/FLF ``.jef`` filter file (big-endian).

        Layout per detector group: int32 dims ``(n_species, n_bins)`` (the
        first group preceded by an int32 count), ``n_species x n_bins`` float64
        filters, then int32 ``n_species`` and ``n_species`` float64 (the photon
        numbers of the species decomposition). ``n_bins`` is the concatenation
        of one segment per routing channel in the order of ``channel_groups``.
        ``tac_dat``: the companion ``*_TAC.dat`` (species patterns and mixture
        per group), stored as patterns when given.
        """
        import struct
        b = open(path, "rb").read()
        pos = 0
        groups = []
        first = True
        while pos < len(b):
            if first:
                _count, ns, nb = struct.unpack(">3i", b[pos:pos + 12])
                pos += 12
                first = False
            else:
                ns, nb = struct.unpack(">2i", b[pos:pos + 8])
                pos += 8
            f = _ffcs_np.frombuffer(b[pos:pos + 8 * ns * nb], dtype=">f8").reshape(ns, nb).astype(_ffcs_np.float64)
            pos += 8 * ns * nb
            (nn,) = struct.unpack(">i", b[pos:pos + 4])
            amps = struct.unpack(">%dd" % nn, b[pos + 4:pos + 4 + 8 * nn])
            pos += 4 + 8 * nn
            groups.append((f, list(amps)))
        if len(groups) != len(channel_groups):
            raise ValueError("%s: %d detector groups in the file, %d given" % (path, len(groups), len(channel_groups)))
        filters = {}
        for (f, _), chans in zip(groups, channel_groups):
            seg = f.shape[1] // len(chans)
            for k, c in enumerate(chans):
                filters[int(c)] = f[:, k * seg:(k + 1) * seg]
        ns = groups[0][0].shape[0]
        patterns = None
        if tac_dat is not None:
            p = _ffcs_np.loadtxt(tac_dat, skiprows=1)
            cols = p.shape[1] // len(channel_groups)          # n_species + mixture per group
            stack = _ffcs_np.concatenate([p[:, g * cols:g * cols + ns] for g in range(len(channel_groups))], axis=0)
            stack = stack / stack.sum(axis=0)
            patterns = {}
            row = 0
            for chans in channel_groups:
                seg = p.shape[0] // len(chans)
                for c in chans:
                    patterns[int(c)] = stack[row:row + seg].T
                    row += seg
        return cls(species or ["species_%d" % (i + 1) for i in range(ns)], filters, micro_time_binning,
                   patterns=patterns,
                   metadata={"source": "legacy SoftCorr/FLF .jef", "channel_groups": [list(g) for g in channel_groups],
                             "species_photon_numbers": [a for _, a in groups]})


def ffcs_correlation(tttr_list, ffcs_filter, species_pair, channel_pairs=((1, 5), (0, 4), (3, 7), (2, 6)),
                     n_bins=8, n_casc=25, method="wahl"):
    """Species cross-correlation of filtered photon streams (SoftCorr "MCS").

    Stream 1 is the photons of the first channel of every pair, weighted with
    the filter of species ``species_pair[0]``; stream 2 the photons of the
    second channels, weighted with species ``species_pair[1]``. The
    correlations of all files of ``tttr_list`` are accumulated over one
    concatenated macro-time axis (files placed end to end). Returns ``(lag in
    seconds, correlation)``.
    """
    i, j = species_pair
    c1 = [a for a, _ in channel_pairs]
    c2 = [b for _, b in channel_pairs]
    t1, w1, t2, w2 = [], [], [], []
    offset = 0
    res = None
    for t in tttr_list:
        mt = _ffcs_np.asarray(t.macro_times, dtype=_ffcs_np.uint64)
        mi = _ffcs_np.asarray(t.micro_times)
        ch = _ffcs_np.asarray(t.routing_channels)
        res = t.header.macro_time_resolution
        w = ffcs_filter.weights(mi, ch)
        s1 = _ffcs_np.isin(ch, c1)
        s2 = _ffcs_np.isin(ch, c2)
        t1.append(mt[s1] + offset); w1.append(w[i, s1])
        t2.append(mt[s2] + offset); w2.append(w[j, s2])
        offset += int(mt[-1]) + 1 if mt.size else 0
    corr = Correlator()
    corr.n_bins, corr.n_casc = n_bins, n_casc
    corr.method = method
    corr.set_events(_ffcs_np.concatenate(t1), _ffcs_np.concatenate(w1), _ffcs_np.concatenate(t2), _ffcs_np.concatenate(w2))
    return _ffcs_np.asarray(corr.x_axis) * res, _ffcs_np.asarray(corr.correlation)
