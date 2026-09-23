# SPDX-License-Identifier: BSD-3-Clause
# Python face of AccurateFretMultiDim.h. Appended to the SWIG module after
# AccurateFret.py and AccurateFretCalibrate.py, whose helpers it uses.


def _afret_nd_dict(r, n_rows):
    np = _afret_np()
    k, d = int(r.n_components), int(r.n_dims)
    return {
        "n_components": k,
        "weights": np.asarray(r.weights, dtype=float),
        "means": np.asarray(r.means, dtype=float).reshape(k, d),
        "sigmas": np.asarray(r.sigmas, dtype=float).reshape(k, d),
        "responsibilities": np.asarray(r.responsibilities, dtype=float).reshape(n_rows, k),
        "labels": np.asarray(r.labels, dtype=int),
        "log_likelihood": float(r.log_likelihood),
        "bic": float(r.bic),
        "n_iter": int(r.n_iter),
        "bic_by_k": {int(a): float(b) for a, b in zip(r.bic_k, r.bic_values)},
    }


def _afret_matrix(x):
    np = _afret_np()
    x = np.asarray(x, dtype=float)
    if x.ndim == 1:
        x = x[:, None]
    return x, int(x.shape[0]), int(x.shape[1])


def gaussian_mixture_nd(x, n_components, *, n_iterations=300, tolerance=1e-7, sigma_floor=1e-3):
    """Diagonal-covariance Gaussian mixture over the columns of ``x`` (n_rows, n_dims).

    Columns are standardised internally; NaNs are missing values and are
    marginalised. Components are sorted by their first-column mean.

    Returns
    -------
    dict
        ``{"n_components", "weights", "means" (k, d), "sigmas" (k, d),
        "responsibilities" (n_rows, k), "labels" (-1 for all-NaN rows),
        "log_likelihood", "bic", "n_iter", "bic_by_k"}``.
    """
    m, n, d = _afret_matrix(x)
    r = _afret_gaussian_mixture_nd(_afret_vec(m), n, d, int(n_components), int(n_iterations),
                                   float(tolerance), float(sigma_floor))
    return _afret_nd_dict(r, n)


def best_gaussian_mixture_nd(x, *, max_components=6, min_weight=0.02):
    """BIC-selected :func:`gaussian_mixture_nd` over 1 .. ``max_components``."""
    m, n, d = _afret_matrix(x)
    return _afret_nd_dict(_afret_best_gaussian_mixture_nd(_afret_vec(m), n, d, int(max_components),
                                                          float(min_weight)), n)


def classify_populations_nd(columns, names=None, *, donor_only_above=0.75,
                            acceptor_only_below=0.25, max_components=6, min_population=20,
                            min_probability=0.9):
    """Donor-only, acceptor-only and FRET bursts from every declared dimension.

    Parameters
    ----------
    columns : mapping or array_like
        ``{name: per-burst array}`` (``names`` then selects and orders them) or
        an ``(n_bursts, len(names))`` array. Names from the declared vocabulary
        (``AFRET_DIMENSIONS``: ``S``, ``E``, ``tau_d``, ``tau_a``, ``r_d``,
        ``r_a``) are interpreted; others only help to separate components.

    Returns
    -------
    dict
        As :func:`classify_es_populations`, with ``"fret_probabilities"``
        ``(n_bursts, n_fret_populations)`` and the component table under
        ``"components"``.
    """
    np = _afret_np()
    if hasattr(columns, "keys"):
        names = list(names or columns.keys())
        m = np.stack([np.asarray(columns[k], dtype=float).ravel() for k in names], axis=1)
    else:
        m, _, _ = _afret_matrix(columns)
        names = list(names)
    r = _afret_classify_populations_nd(_afret_vec(m), int(m.shape[0]), VectorString(names),
                                       float(donor_only_above), float(acceptor_only_below),
                                       int(max_components), int(min_population),
                                       float(min_probability))
    return _afret_split_dict(r)
