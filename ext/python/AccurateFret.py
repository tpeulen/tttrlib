# SPDX-License-Identifier: BSD-3-Clause
# Python face of AccurateFret.h: numpy in, dicts of numpy arrays out. Appended to
# the SWIG module, so the raw `_afret_*` kernels and the structs are in scope.


def _afret_np():
    import numpy
    return numpy


def _afret_vec(x):
    np = _afret_np()
    return np.ascontiguousarray(x, dtype=float).ravel()


def _afret_factors(**values):
    f = FretFactors()
    for key, value in values.items():
        if value is not None:
            setattr(f, key, float(value))
    return f


def apparent_es(i_dd, i_da, i_aa=None):
    """Apparent (uncorrected) proximity ratio ``E`` and raw stoichiometry ``S``.

    Parameters
    ----------
    i_dd, i_da : array_like
        Per-burst donor and acceptor counts under donor excitation.
    i_aa : array_like, optional
        Per-burst acceptor counts under acceptor excitation (ALEX/PIE).

    Returns
    -------
    dict
        ``{"E": ndarray, "S": ndarray or None}``, broadcast to the common
        shape of the inputs. ``E = i_da/(i_dd+i_da)``, ``S = (i_dd+i_da)/
        (i_dd+i_da+i_aa)``, each 0 where its denominator is 0.
    """
    np = _afret_np()
    arrays = [i_dd, i_da] + ([] if i_aa is None else [i_aa])
    arrays = np.broadcast_arrays(*[np.asarray(a, dtype=float) for a in arrays])
    shape = arrays[0].shape
    r = _afret_apparent_es(*[_afret_vec(a) for a in arrays])
    return {
        "E": np.asarray(r.E, dtype=float).reshape(shape),
        "S": np.asarray(r.S, dtype=float).reshape(shape) if i_aa is not None else None,
    }


def corrected_es(i_dd, i_da, i_aa=None, *, gamma=1.0, alpha=0.0, beta=1.0, delta=0.0,
                 bg_dd=0.0, bg_da=0.0, bg_aa=0.0):
    """Fully corrected per-burst FRET efficiency ``E`` and stoichiometry ``S``.

    ``F_dd = i_dd - bg_dd``, ``F_aa = i_aa - bg_aa``,
    ``F_da = i_da - bg_da - alpha F_dd - delta F_aa``,
    ``E = F_da / (F_da + gamma F_dd)`` (0 where that denominator is <= 0) and
    ``S = (gamma F_dd + F_da) / (gamma F_dd + F_da + F_aa / beta)`` (0 where
    its denominator is 0), after Hellenkamp et al. 2018.

    Parameters
    ----------
    i_dd, i_da : array_like
        Per-burst counts under donor excitation.
    i_aa : array_like, optional
        Per-burst acceptor counts under acceptor excitation. Without it
        ``F_aa`` is 0, ``delta`` has no effect and ``S`` is ``None``.
    gamma, alpha, beta, delta : float
        Correction factors.
    bg_dd, bg_da, bg_aa : float
        Per-burst channel backgrounds.

    Returns
    -------
    dict
        ``{"E", "S", "fc"}``; ``fc`` is the corrected sensitised emission
        ``F_da``. Arrays take the broadcast shape of the inputs.
    """
    np = _afret_np()
    arrays = [i_dd, i_da] + ([] if i_aa is None else [i_aa])
    arrays = np.broadcast_arrays(*[np.asarray(a, dtype=float) for a in arrays])
    shape = arrays[0].shape
    flat = [_afret_vec(a) for a in arrays]
    if i_aa is None:
        flat.append(np.zeros(0))
    f = _afret_factors(gamma=gamma, alpha=alpha, beta=beta, delta=delta,
                       bg_dd=bg_dd, bg_da=bg_da, bg_aa=bg_aa)
    r = _afret_corrected_es(flat[0], flat[1], flat[2], f)
    return {
        "E": np.asarray(r.E, dtype=float).reshape(shape),
        "S": np.asarray(r.S, dtype=float).reshape(shape) if i_aa is not None else None,
        "fc": np.asarray(r.fc, dtype=float).reshape(shape),
    }


def _afret_pairs_out(r, trailing):
    np = _afret_np()
    n_pairs = len(r.donor)
    e = np.asarray(r.E, dtype=float).reshape((n_pairs,) + trailing)
    fc = np.asarray(r.fc, dtype=float).reshape((n_pairs,) + trailing)
    return {(int(i), int(j)): {"E": e[p], "fc": fc[p]}
            for p, (i, j) in enumerate(zip(r.donor, r.acceptor))}


def _afret_flat_pairs(pairs):
    return [] if pairs is None else [int(v) for pair in pairs for v in pair]


def corrected_es_matrix(intensity, gamma, alpha, delta=None, background=None, pairs=None):
    """Corrected pairwise FRET efficiencies for an N-chromophore system.

    ``F_ij = (I_ij - Bg_ij) - alpha_ij (I_ii - Bg_ii) - delta_ij (I_jj - Bg_jj)``
    and ``E_ij = (F_ij/gamma_ij) / (F_ii + sum_k F_ik/gamma_ik)``: the donor
    budget is shared by all of its acceptors.

    Parameters
    ----------
    intensity : array_like
        ``(N, N)`` or ``(N, N, ...)``: ``I[i, j]`` is chromophore ``j``'s signal
        under excitation of chromophore ``i``; trailing axes are bursts.
    gamma, alpha : array_like
        ``(N, N)`` factor matrices.
    delta, background : array_like, optional
        ``(N, N)`` matrices, zeros when omitted.
    pairs : sequence of (int, int), optional
        Donor/acceptor index pairs; every ``i < j`` when omitted.

    Returns
    -------
    dict
        ``{(i, j): {"E": ndarray, "fc": ndarray}}`` with the trailing shape
        of ``intensity``.
    """
    np = _afret_np()
    inten = np.asarray(intensity, dtype=float)
    n = inten.shape[0]
    trailing = inten.shape[2:]
    n_bursts = int(np.prod(trailing)) if trailing else 1
    opt = lambda m: [] if m is None else _afret_vec(m)
    r = _afret_corrected_es_matrix(
        _afret_vec(inten), n, n_bursts, _afret_vec(gamma), _afret_vec(alpha),
        opt(delta), opt(background), _afret_flat_pairs(pairs))
    return _afret_pairs_out(r, trailing)


def corrected_es_general(intensity, excitation, emission, *, background=None, pairs=None,
                         unmix="naive", ridge=0.0):
    """Corrected pairwise FRET efficiencies from the light-path crosstalk matrices.

    Un-mixes the emission (``I[l, :] = e[l, :] @ emission``), subtracts the
    directly excited acceptor emission ``(excitation[l, k]/excitation[k, k])
    e[k, k]`` and divides by the coupled donor budget ``e[l, l] + sum_a F[l, a]``.
    Reduces to :func:`corrected_es` for two colours with ``emission = [[1,
    alpha], [0, gamma]]`` and ``excitation = [[1, delta], [0, 1]]``.

    Parameters
    ----------
    intensity : array_like
        ``(L, M)`` or ``(L, M, ...)`` measured signal of channel ``m`` under
        laser ``l``; trailing axes are bursts.
    excitation : array_like
        ``(L, N)`` excitation crosstalk matrix.
    emission : array_like
        ``(N, M)`` detected-brightness matrix.
    background : array_like, optional
        Broadcastable to ``(L, M)``; subtracted from ``intensity``.
    pairs : sequence of (int, int), optional
        Donor/acceptor pairs; every ``l < k`` when omitted.
    unmix : {"naive", "stable"}
        ``"naive"`` (aliases ``"pinv"``, ``"linear"``): pseudo-inverse or, with
        ``ridge > 0``, the Tikhonov solution. ``"stable"`` (``"nnls"``,
        ``"nonneg"``): non-negative least squares per burst.
    ridge : float
        Tikhonov strength, 0 for none.

    Returns
    -------
    dict
        ``{(l, k): {"E": ndarray, "fc": ndarray}}`` with the trailing shape of
        ``intensity``.
    """
    np = _afret_np()
    inten = np.asarray(intensity, dtype=float)
    emis = np.asarray(emission, dtype=float)
    L, M = inten.shape[0], inten.shape[1]
    trailing = inten.shape[2:]
    n_bursts = int(np.prod(trailing)) if trailing else 1
    bg = [] if background is None else _afret_vec(np.broadcast_to(np.asarray(background, dtype=float), (L, M)))
    r = _afret_corrected_es_general(
        _afret_vec(inten), L, M, n_bursts, _afret_vec(excitation), _afret_vec(emis),
        emis.shape[0], bg, _afret_flat_pairs(pairs), str(unmix), float(ridge))
    return _afret_pairs_out(r, trailing)
