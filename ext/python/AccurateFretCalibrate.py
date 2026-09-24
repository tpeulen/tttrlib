# SPDX-License-Identifier: BSD-3-Clause
# Python face of AccurateFretCalibrate.h: auto_calibrate(columns, constants, options).
# Appended to the SWIG module after AccurateFret.py, whose helpers it uses.

_AFRET_FACTOR_ALIASES = {
    "gamma": ("gamma",), "alpha": ("alpha",), "beta": ("beta",), "delta": ("delta",),
    "bg_dd": ("bg_dd", "Bg_DD"), "bg_da": ("bg_da", "Bg_DA"), "bg_aa": ("bg_aa", "Bg_AA"),
    "r0": ("r0", "R0"),
}

_AFRET_OPTION_KEYS = (
    "gamma_source", "n_iterations", "tolerance", "n_bootstrap", "seed", "use_priors",
    "assume_one_to_one", "min_population", "max_fret_populations", "donor_only_above",
    "acceptor_only_below", "donor_lifetime", "min_probability", "max_components_nd",
    "species_factors", "sigma_model", "population_method", "hdbscan_min_cluster_fraction",
    "hdbscan_min_cluster_size", "hdbscan_min_samples", "hdbscan_max_points",
    "hdbscan_selection", "remove_outliers", "outlier_es_lo", "outlier_es_hi",
    "outlier_tau_max", "outlier_r_lo", "outlier_r_hi", "outlier_fence", "outlier_quantile", "e_min_significance",
)

#: The population finders of the multidimensional gating (``population_method``).
AFRET_POPULATION_METHODS = {
    "hdbscan": "HDBSCAN (density-based; clusters of any shape, sparse bursts are noise)",
    "gmm": "Gaussian mixture (BIC-selected, diagonal covariances)",
}

#: The declared per-burst dimensions the multidimensional gating understands:
#: name -> (column keys accepted in ``columns``, meaning). "S" and "E" are
#: computed from the counts with the current factors on every pass.
AFRET_DIMENSIONS = {
    "S": ((), "corrected stoichiometry"),
    "E": ((), "corrected FRET efficiency"),
    "tau_d": (("tau_d", "tau_f"), "donor fluorescence lifetime, ns"),
    "tau_a": (("tau_a",), "acceptor fluorescence lifetime, ns"),
    "r_d": (("r_d",), "donor steady-state anisotropy"),
    "r_a": (("r_a",), "acceptor steady-state anisotropy"),
}


def _afret_dimension_column(columns, name):
    keys = AFRET_DIMENSIONS.get(name, ((name,), ""))[0]
    for key in keys:
        if columns.get(key) is not None:
            return _afret_vec(columns[key])
    raise ValueError(f"auto_calibrate: dimension {name!r} was declared but no column "
                     f"{' or '.join(map(repr, keys))} was given")


def _afret_factor_dict(f):
    return {k: float(getattr(f, k)) for k in _AFRET_FACTOR_ALIASES}


def _afret_factors_from(constants):
    f = FretFactors()
    for key, names in _AFRET_FACTOR_ALIASES.items():
        for name in names:
            if constants.get(name) is not None:
                setattr(f, key, float(constants[name]))
                break
    return f


def _afret_options(constants, options):
    o = AutoCalibrateOptions()
    for key in _AFRET_OPTION_KEYS:
        if options.get(key) is not None:
            setattr(o, key, type(getattr(o, key))(options[key]))
    for name, prior in (constants.get("priors") or {}).items():
        if prior is None:
            continue
        mu, sigma = prior
        setattr(o, name + "_prior_mu", float(mu))
        setattr(o, name + "_prior_sigma", float(sigma))
    if options.get("dimensions"):
        o.dimensions = VectorString([str(d) for d in options["dimensions"]])
    lt, le = _afret_line(options.get("line"))
    o.line_tau_f = VectorDouble([float(v) for v in lt])
    o.line_efficiency = VectorDouble([float(v) for v in le])
    for key, value in (options.get("bounds") or {}).items():
        lo, hi = value
        setattr(o, key + "_lo", float(lo))
        setattr(o, key + "_hi", float(hi))
    return o


def _afret_calibration_dict(st):
    estimates = {"es": st.gamma_es, "lifetime": st.gamma_lifetime, "prior": st.gamma_prior,
                 "posterior": st.gamma_posterior, "data": st.gamma_data}
    if st.has_lifetime_gamma:
        estimates["lifetime_sigma"] = st.gamma_lifetime_sigma
    return {
        "factors": _afret_factor_dict(st.factors),
        "uncertainties": {"alpha": st.sigma_alpha, "delta": st.sigma_delta, "gamma": st.sigma_gamma,
                          "beta": st.sigma_beta, "r0": st.sigma_r0},
        "gamma_estimates": {k: float(v) for k, v in estimates.items()},
        "estimated": {"alpha": bool(st.estimated_alpha), "delta": bool(st.estimated_delta),
                      "gamma": bool(st.estimated_gamma)},
        "split": _afret_split_dict(st.split) if st.has_split else None,
        "outliers": _afret_outliers_dict(st.split) if st.has_split else None,
        "tau_d0": float(st.tau_d0),
        "tau_a": float(st.tau_a),
        "dimensions": list(st.split.dimensions) if st.has_split else [],
        "species": _afret_species_dict(st) if st.has_species else None,
        "populations": [_afret_population_dict(p) for p in st.populations],
        "iterations": int(st.iterations),
        "converged": bool(st.converged),
        "cancelled": bool(st.cancelled),
        "messages": list(st.messages),
    }


def _afret_outliers_dict(r):
    """The pre-cleaning of the gating dimensions: ``None`` when it did not run."""
    np = _afret_np()
    if not len(r.outlier):
        return None
    mask = np.asarray(r.outlier, dtype=bool)
    by = {str(name): {"range": int(a), "fence": int(b), "lo": float(lo), "hi": float(hi)}
          for name, a, b, lo, hi in zip(r.outlier_dimensions, r.outlier_range, r.outlier_fence,
                                        r.outlier_lo, r.outlier_hi)}
    return {"n": int(mask.sum()), "mask": mask, "by_dimension": by}


def auto_calibrate(columns, constants=None, options=None, progress=None):
    """Determine all FRET correction factors automatically from one measurement.

    Iterated to self-consistency: correct E/S with the current factors, find
    the donor-only, acceptor-only and FRET bursts with a stoichiometry
    mixture, take alpha from the donor-only and delta from the acceptor-only
    class, gamma and beta from the ``1/S`` vs ``E`` line over the FRET
    sub-populations (or gamma from the donor lifetime and a static FRET line,
    and beta from ``S = 0.5``), and finally combine gamma/alpha/delta with
    their light-path priors.

    Parameters
    ----------
    columns : mapping
        Per-burst arrays: ``"i_dd"``, ``"i_da"`` and optionally ``"i_aa"``
        (acceptor excitation; without it every burst is taken to be doubly
        labelled) and ``"tau_f"`` (donor lifetime, ns).
    constants : mapping, optional
        Starting factors ``gamma, alpha, beta, delta, bg_dd, bg_da, bg_aa, r0``
        (``Bg_DD``/``R0`` spellings accepted) and ``"priors"``:
        ``{"gamma"|"alpha"|"delta": (mu, sigma)}`` light-path priors.
    options : mapping, optional
        ``gamma_source`` ("auto", "es", "lifetime", "combined"),
        ``n_iterations``, ``tolerance``, ``n_bootstrap``, ``seed``,
        ``use_priors``, ``assume_one_to_one``, ``min_population``,
        ``max_fret_populations``, ``donor_only_above``, ``acceptor_only_below``,
        ``dimensions`` (gating dimensions, see ``AFRET_DIMENSIONS``),
        ``population_method`` ("hdbscan" or "gmm": the finder of the
        multidimensional gating), ``hdbscan_min_cluster_fraction``,
        ``hdbscan_min_cluster_size``, ``hdbscan_min_samples`` (0: automatic),
        ``hdbscan_max_points``, ``hdbscan_selection`` ("leaf" or "eom"),
        ``remove_outliers`` and its limits ``outlier_es_lo``/``outlier_es_hi``,
        ``outlier_tau_max``, ``outlier_r_lo``/``outlier_r_hi``,
        ``outlier_fence`` and ``outlier_quantile`` (fence beyond the quantile range),
        ``e_min_significance``,
        ``line`` (static FRET line, ``(tau_f, E)`` or an object with those
        attributes), ``bounds`` (``{"gamma": (lo, hi), ...}``) and
        ``bootstrap_indices``: a callable ``draw(size) -> positions`` used for
        every class of every resample (donor-only, acceptor-only, FRET, in that
        order); without it tttrlib's own generator draws them from ``seed``.
    progress : callable, optional
        ``progress(step, total, message)`` after each pass; returning
        ``False`` stops the calibration after that pass.

    Returns
    -------
    dict
        ``factors``, ``uncertainties`` (NaN when not estimated),
        ``gamma_estimates`` (``es``, ``lifetime``, ``prior``, ``data``,
        ``posterior``), ``estimated`` (which factors the data identified),
        ``split`` (masks, FRET labels, cuts; ``noise`` for HDBSCAN),
        ``outliers`` (``{"n", "mask", "by_dimension": {dim: {"range", "fence",
        "lo", "hi"}}}``, ``None`` without dimensions), ``populations``, ``iterations``,
        ``converged``, ``cancelled`` and ``messages``.
    """
    constants = dict(constants or {})
    options = dict(options or {})
    o = _afret_options(constants, options)
    vec = lambda key: [] if columns.get(key) is None else _afret_vec(columns[key])
    tau = vec("tau_f") if columns.get("tau_f") is not None else vec("tau_d")
    st = _afret_auto_calibrate_start(vec("i_dd"), vec("i_da"), vec("i_aa"), tau,
                                     _afret_factors_from(constants), o)
    extra = [d for d in (options.get("dimensions") or []) if d not in ("S", "E")]
    if extra:
        np = _afret_np()
        matrix = np.stack([_afret_dimension_column(columns, d) for d in extra], axis=1)
        _afret_auto_calibrate_set_dimensions(st, _afret_vec(matrix), VectorString(extra))
    total = int(o.n_iterations) + int(o.n_bootstrap)
    it = 0
    for it in range(1, int(o.n_iterations) + 1):
        small = _afret_auto_calibrate_iterate(st, o)
        if progress is not None and progress(
                it, total, f"refining the correction factors (pass {it})") is False:
            _afret_auto_calibrate_cancel(st)
            break
        if small:
            st.converged = True
            break
    if not st.cancelled and int(o.n_bootstrap) > 0:
        _afret_run_bootstrap(st, o, options.get("bootstrap_indices"), progress, it, total)
    _afret_auto_calibrate_finish(st, o)
    return _afret_calibration_dict(st)


def _afret_run_bootstrap(st, o, draw, progress, step, total):
    """Resample the three classes ``n_bootstrap`` times (positions drawn by ``draw(size)``
    in the order donor-only, acceptor-only, FRET, or by tttrlib's generator)."""
    np = _afret_np()
    sizes = [int(np.count_nonzero(st.split.donor_only)),
             int(np.count_nonzero(st.split.acceptor_only)) if len(st.data_aa) else 0,
             int(np.count_nonzero(st.split.fret))]
    n = int(o.n_bootstrap)
    for r in range(n):
        step += 1
        if progress is not None and progress(
                step, total, f"bootstrapping the uncertainties ({r + 1}/{n})") is False:
            break
        pos = [[] if (draw is None or size == 0) else [int(v) for v in draw(size)] for size in sizes]
        _afret_auto_calibrate_bootstrap(st, o, *pos)


def _afret_population_dict(p):
    entry = {"label": int(p.label), "n": int(p.n), "E": p.E, "sigma_E": p.sigma_E,
             "sigma_E_statistical": p.sigma_E_statistical,
             "sigma_E_systematic": p.sigma_E_systematic, "S": p.S, "distance": p.distance,
             "sigma_distance": p.sigma_distance}
    if p.has_tau:
        entry["tau_f"] = p.tau_f
    if p.has_line:
        entry["E_line"] = p.E_line
        entry["deviation"] = p.deviation
    return entry


def efficiency_uncertainty(efficiency, f_dd, f_aa=None, *, gamma=1.0, sigma_gamma=0.0,
                           sigma_alpha=0.0, sigma_delta=0.0, sigma_statistical=None):
    """Propagate the gamma/alpha/delta uncertainties into the efficiency.

    ``dE/dgamma = -E(1-E)/gamma``, ``dE/dalpha = -(1-E)^2/gamma``,
    ``dE/ddelta = -(1-E)^2 F_aa/(gamma F_dd)``; the systematic error is their
    quadrature sum, the total adds ``sigma_statistical``.

    Returns
    -------
    dict
        ``{"total", "systematic", "statistical", "terms": {"gamma", "alpha", "delta"}}``.
    """
    np = _afret_np()
    e = np.asarray(efficiency, dtype=float)
    shape = e.shape
    arrays = [e, np.asarray(f_dd, dtype=float)] + ([] if f_aa is None else [np.asarray(f_aa, dtype=float)])
    if sigma_statistical is not None:
        arrays.append(np.asarray(sigma_statistical, dtype=float))
    arrays = np.broadcast_arrays(*arrays)
    shape = arrays[0].shape
    flat = [_afret_vec(a) for a in arrays]
    aa = flat[2] if f_aa is not None else []
    st = flat[-1] if sigma_statistical is not None else []
    r = _afret_efficiency_uncertainty(flat[0], flat[1], aa, float(gamma or 0.0), float(sigma_gamma),
                                      float(sigma_alpha), float(sigma_delta), st)
    out = lambda v: np.asarray(v, dtype=float).reshape(shape)
    return {"total": out(r.total), "systematic": out(r.systematic), "statistical": out(r.statistical),
            "terms": {"gamma": out(r.d_gamma), "alpha": out(r.d_alpha), "delta": out(r.d_delta)}}


def distance_from_efficiency(efficiency, r0, *, sigma_efficiency=None, sigma_r0=0.0):
    """``R = R0 (1/E - 1)^(1/6)`` and its error; NaN outside ``0 < E < 1``.

    Returns
    -------
    dict
        ``{"distance", "sigma"}`` in the unit of ``r0``, shaped like ``efficiency``.
    """
    np = _afret_np()
    e = np.asarray(efficiency, dtype=float)
    se = [] if sigma_efficiency is None else _afret_vec(np.broadcast_to(sigma_efficiency, e.shape))
    r = _afret_distance_from_efficiency(_afret_vec(e), float(r0), se, float(sigma_r0))
    return {"distance": np.asarray(r.distance, dtype=float).reshape(e.shape),
            "sigma": np.asarray(r.sigma, dtype=float).reshape(e.shape)}


def accurate_fret(i_dd, i_da, i_aa=None, *, factors=None, uncertainties=None, tau_f=None,
                  line=None, labels=None):
    """Accurate per-burst E and S, their errors, distances and population summaries.

    Parameters
    ----------
    factors : mapping, optional
        ``gamma, alpha, beta, delta, bg_dd, bg_da, bg_aa, r0`` (``Bg_DD``/``R0``
        accepted); uncorrected defaults otherwise.
    uncertainties : mapping, optional
        ``{"gamma", "alpha", "delta", "r0"}`` standard uncertainties; missing
        or non-finite ones count as 0.
    tau_f, line : optional
        Per-burst donor lifetime and a static FRET line (``(tau_f, E)`` or an
        object with those attributes) for the deviation from the line.
    labels : array_like, optional
        Population label per burst; one population when omitted.

    Returns
    -------
    dict
        ``{"E", "S", "fc", "sigma_E", "sigma_E_systematic", "distance",
        "sigma_distance", "deviation", "populations", "calibration"}``.
    """
    np = _afret_np()
    f = _afret_factors_from(dict(factors or {}))
    u = {"gamma": 0.0, "alpha": 0.0, "delta": 0.0, "r0": 0.0}
    for key, value in (uncertainties or {}).items():
        if key in u and value is not None:
            u[key] = float(value)
    lt, le = _afret_line(line)
    vec = lambda v: [] if v is None else _afret_vec(v)
    lab = [] if labels is None else [int(v) for v in np.asarray(labels).ravel()]
    r = _afret_accurate_fret(_afret_vec(i_dd), _afret_vec(i_da), vec(i_aa), f, u["gamma"], u["alpha"],
                             u["delta"], u["r0"], vec(tau_f), lt, le, lab)
    arr = lambda v: np.asarray(v, dtype=float)
    return {"E": arr(r.es.E), "S": arr(r.es.S) if i_aa is not None else None, "fc": arr(r.es.fc),
            "sigma_E": arr(r.sigma_E), "sigma_E_systematic": arr(r.sigma_E_systematic),
            "distance": arr(r.distance), "sigma_distance": arr(r.sigma_distance),
            "deviation": arr(r.deviation) if r.has_deviation else None,
            "populations": [_afret_population_dict(p) for p in r.populations],
            "calibration": _afret_factor_dict(r.factors)}


def refine_gamma(i_dd, i_da, i_aa, labels, *, alpha=0.0, delta=0.0, prior=None, data_sigma=None,
                 n_bootstrap=60, seed=0, indices=None):
    """Precision-weighted gamma from the 1/S vs E fit and a ``(mu, sigma)`` prior.

    ``indices`` (``(n_bootstrap, n_bursts)``) makes the bootstrap of the data
    uncertainty reproducible from outside; tttrlib's generator otherwise.

    Returns
    -------
    dict
        ``{"gamma_data", "beta", "data_sigma", "gamma_prior", "gamma_posterior"}``.
    """
    np = _afret_np()
    mu, sigma = prior if prior is not None else (0.0, -1.0)
    idx = [] if indices is None else [int(v) for v in np.asarray(indices).ravel()]
    r = _afret_refine_gamma(_afret_vec(i_dd), _afret_vec(i_da), _afret_vec(i_aa),
                            [int(v) for v in np.asarray(labels).ravel()], float(alpha), float(delta),
                            float(mu), float(sigma), -1.0 if data_sigma is None else float(data_sigma),
                            int(n_bootstrap), int(seed), idx)
    keys = ("gamma_data", "beta", "data_sigma", "gamma_prior", "gamma_posterior")
    return {k: float(v) for k, v in zip(keys, r)}


def rcm_from_dye_solutions(donor_sample_rates, acceptor_sample_rates, absorbance_ratio,
                           detector_assignment, anisotropy=(0.0, 0.0)):
    """Routing/detection-correction matrix (RCM) from dye-solution measurements.

    Port of Fretica ``FRCMCalibrationFromDyeSolutions``.

    Parameters
    ----------
    donor_sample_rates, acceptor_sample_rates : array_like
        Background-corrected count rate in every channel for the donor-only and
        the acceptor-only dye solution.
    absorbance_ratio : float
        ``AbsorbanceA / AbsorbanceD`` of the two solutions.
    detector_assignment : sequence of (str, str)
        Per channel ``(species, polarisation)``, species ``"A"``/``"D"``,
        polarisation ``"P"``/``"S"`` (ignored for two channels).
    anisotropy : (float, float)
        ``(r_donor, r_acceptor)``, used with a polarising beam splitter.

    Returns
    -------
    numpy.ndarray
        The ``(n, n)`` correction matrix (identity on unused channels,
        normalised so its first ordered element is 1).
    """
    np = _afret_np()
    species = [str(d[0]) for d in detector_assignment]
    polarisation = [str(d[1]) if len(d) > 1 else "" for d in detector_assignment]
    n = len(species)
    r = _afret_rcm_from_dye_solutions(_afret_vec(donor_sample_rates), _afret_vec(acceptor_sample_rates),
                                      float(absorbance_ratio), VectorString(species),
                                      VectorString(polarisation), float(anisotropy[0]),
                                      float(anisotropy[1]))
    return np.asarray(r, dtype=float).reshape(n, n)
