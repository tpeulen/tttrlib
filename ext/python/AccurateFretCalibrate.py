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
    "acceptor_only_below",
)


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
        "populations": [],
        "iterations": int(st.iterations),
        "converged": bool(st.converged),
        "cancelled": bool(st.cancelled),
        "messages": list(st.messages),
    }


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
        ``line`` (static FRET line, ``(tau_f, E)`` or an object with those
        attributes) and ``bounds`` (``{"gamma": (lo, hi), ...}``).
    progress : callable, optional
        ``progress(step, total, message)`` after each pass; returning
        ``False`` stops the calibration after that pass.

    Returns
    -------
    dict
        ``factors``, ``uncertainties`` (NaN when not estimated),
        ``gamma_estimates`` (``es``, ``lifetime``, ``prior``, ``data``,
        ``posterior``), ``estimated`` (which factors the data identified),
        ``split`` (masks, FRET labels, cuts), ``populations``, ``iterations``,
        ``converged``, ``cancelled`` and ``messages``.
    """
    constants = dict(constants or {})
    options = dict(options or {})
    o = _afret_options(constants, options)
    vec = lambda key: [] if columns.get(key) is None else _afret_vec(columns[key])
    st = _afret_auto_calibrate_start(vec("i_dd"), vec("i_da"), vec("i_aa"), vec("tau_f"),
                                     _afret_factors_from(constants), o)
    total = int(o.n_iterations) + int(o.n_bootstrap)
    for it in range(1, int(o.n_iterations) + 1):
        small = _afret_auto_calibrate_iterate(st, o)
        if progress is not None and progress(
                it, total, f"refining the correction factors (pass {it})") is False:
            _afret_auto_calibrate_cancel(st)
            break
        if small:
            st.converged = True
            break
    _afret_auto_calibrate_finish(st, o)
    return _afret_calibration_dict(st)
