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


def _afret_mixture_dict(r, n_finite):
    np = _afret_np()
    k = int(r.n_components)
    out = {
        "weights": np.asarray(r.weights, dtype=float),
        "means": np.asarray(r.means, dtype=float),
        "sigmas": np.asarray(r.sigmas, dtype=float),
        "responsibilities": np.asarray(r.responsibilities, dtype=float).reshape(n_finite, k),
        "labels": np.asarray(r.labels, dtype=int),
        "log_likelihood": float(r.log_likelihood),
        "bic": float(r.bic),
        "n_iter": int(r.n_iter),
    }
    if len(r.bic_k):
        out["bic_by_k"] = {int(k_): float(b) for k_, b in zip(r.bic_k, r.bic_values)}
    return out


def gaussian_mixture_1d(x, n_components, *, n_iterations=300, tolerance=1e-7,
                        sigma_floor=1e-3, init="auto"):
    """Fit a deterministic one-dimensional Gaussian mixture by EM.

    Quantile-spaced and range-spaced starts are both run (``init="auto"``) and
    the higher log-likelihood is kept. Non-finite samples are ignored.

    Returns
    -------
    dict
        ``{"weights", "means", "sigmas", "responsibilities", "labels",
        "log_likelihood", "bic", "n_iter"}``, components sorted by mean;
        ``responsibilities`` is ``(n_finite, k)`` and ``labels`` the most
        likely component of each finite sample.
    """
    v = _afret_vec(x)
    n_finite = int(_afret_np().count_nonzero(_afret_np().isfinite(v)))
    r = _afret_gaussian_mixture_1d(v, int(n_components), int(n_iterations), float(tolerance),
                                   float(sigma_floor), str(init))
    return _afret_mixture_dict(r, n_finite)


def best_gaussian_mixture_1d(x, *, max_components=4, min_weight=0.02):
    """BIC-selected :func:`gaussian_mixture_1d`, with ``"bic_by_k"`` of every fit tried."""
    v = _afret_vec(x)
    n_finite = int(_afret_np().count_nonzero(_afret_np().isfinite(v)))
    r = _afret_best_gaussian_mixture_1d(v, int(max_components), float(min_weight))
    out = _afret_mixture_dict(r, n_finite)
    out.setdefault("bic_by_k", {})
    return out


def _afret_split_dict(r):
    np = _afret_np()
    components = {}
    if len(r.component_means):
        components = {
            "means": list(r.component_means),
            "weights": list(r.component_weights),
            "sigmas": list(r.component_sigmas),
            "bic_by_k": {int(k): float(b) for k, b in zip(r.bic_k, r.bic_values)},
        }
    fret_labels = np.asarray(r.fret_labels, dtype=int)
    fret = np.asarray(r.fret, dtype=bool)
    donor_only = np.asarray(r.donor_only, dtype=bool)
    acceptor_only = np.asarray(r.acceptor_only, dtype=bool)
    return {
        "donor_only": donor_only,
        "acceptor_only": acceptor_only,
        "fret": fret,
        "fret_labels": fret_labels,
        "thresholds": (float(r.threshold_lo), float(r.threshold_hi)),
        "method": str(r.method),
        "components": components,
        "counts": {
            "donor_only": int(donor_only.sum()),
            "acceptor_only": int(acceptor_only.sum()),
            "fret": int(fret.sum()),
            "fret_populations": int(len(np.unique(fret_labels[fret_labels >= 0]))),
        },
    }


def classify_es_populations(stoichiometry, efficiency=None, *, donor_only_above=0.75,
                            acceptor_only_below=0.25, max_components=4, max_fret_populations=3,
                            min_population=20, reference_sigma=2.0, method="auto"):
    """Find donor-only, acceptor-only and FRET bursts without manual gates.

    A BIC-selected Gaussian mixture over the stoichiometry assigns each
    component to a class by its centre; the FRET class is cut at the midpoints
    between neighbouring components, the reference classes at
    ``reference_sigma`` widths of their own component. ``method="threshold"``
    uses the fixed cuts. With ``efficiency`` the FRET bursts are split into
    sub-populations (:func:`split_fret_subpopulations`).

    Returns
    -------
    dict
        ``{"donor_only", "acceptor_only", "fret"}`` boolean masks,
        ``"fret_labels"`` (-1 outside the FRET class), ``"thresholds"``
        ``(lo, hi)``, ``"method"``, ``"components"`` and ``"counts"``.
    """
    s = _afret_vec(stoichiometry)
    e = [] if efficiency is None else _afret_vec(efficiency)
    r = _afret_classify_es_populations(
        s, e, float(donor_only_above), float(acceptor_only_below), int(max_components),
        int(max_fret_populations), int(min_population), float(reference_sigma), str(method))
    return _afret_split_dict(r)


def split_fret_subpopulations(efficiency, *, max_populations=3, min_population=20,
                              min_separation=0.05, min_fraction=0.1):
    """Sub-population index per doubly labelled burst, by increasing efficiency."""
    np = _afret_np()
    return np.asarray(_afret_split_fret_subpopulations(
        _afret_vec(efficiency), int(max_populations), int(min_population),
        float(min_separation), float(min_fraction)), dtype=int)


def leakage_from_donor_only(i_dd, i_da, *, bg_dd=0.0, bg_da=0.0):
    """Donor leakage ``alpha = <i_da - bg_da> / <i_dd - bg_dd>`` of donor-only bursts."""
    return float(_afret_leakage_from_donor_only(_afret_vec(i_dd), _afret_vec(i_da),
                                                float(bg_dd), float(bg_da)))


def direct_excitation_from_acceptor_only(i_da, i_aa, i_dd=None, *, alpha=0.0, bg_dd=0.0,
                                         bg_da=0.0, bg_aa=0.0):
    """Direct excitation ``delta = <i_da - bg_da - alpha (i_dd - bg_dd)> / <i_aa - bg_aa>``."""
    dd = [] if i_dd is None else _afret_vec(i_dd)
    return float(_afret_direct_excitation_from_acceptor_only(
        _afret_vec(i_da), _afret_vec(i_aa), dd, float(alpha), float(bg_dd), float(bg_da),
        float(bg_aa)))
