# SPDX-License-Identifier: BSD-3-Clause
# Python face of AccurateFretSpecies.h: the per-population factor table that
# auto_calibrate returns under "species". Appended after AccurateFretCalibrate.py.


def _afret_species_dict(st):
    """Global + per-population factors, assignment probabilities and the model choice.

    Shaped for a parameter table with vector-valued constants: every factor
    has one ``global`` value and one ``values``/``sigma`` entry per FRET
    population, ``pooled`` says whether the populations share it, and
    ``assignment[b, s]`` is burst ``b``'s probability of population ``s``.
    """
    np = _afret_np()
    sp = st.species
    P = int(sp.n_populations)
    n = len(st.data_dd)
    split = st.split
    if P and len(split.fret_probabilities):
        assignment = np.asarray(split.fret_probabilities, dtype=float).reshape(n, P)
    else:
        assignment = np.zeros((n, P))
        labels = np.asarray(split.fret_labels, dtype=int)
        fret = np.asarray(split.fret, dtype=bool) & (labels >= 0)
        assignment[np.flatnonzero(fret), labels[fret]] = 1.0
    f = st.factors
    arr = lambda v: [float(x) for x in v]
    species = str(sp.selected) == "species"
    pooled = lambda value, sigma: {"global": float(value), "values": [float(value)] * P,
                                   "sigma": [float(sigma)] * P, "pooled": True}
    populations = [
        {"label": s, "name": f"FRET {s + 1}", "n": float(sp.n_eff[s]), "E": float(sp.E_pop[s]),
         "S": float(sp.S_pop[s]), "tau_d": float(sp.tau_d[s]), "E_line": float(sp.e_line[s]),
         "tau_a": float(sp.tau_a[s])}
        for s in range(P)]
    return {
        "populations": populations,
        "labels": list(range(P)),
        "names": [p["name"] for p in populations],
        "factors": {
            "gamma": {"global": float(f.gamma), "values": arr(sp.gamma),
                      "sigma": arr(sp.sigma_gamma) if species else [float(st.sigma_gamma)] * P,
                      "pooled": not species},
            "beta": pooled(sp.beta, sp.sigma_beta if species else st.sigma_beta),
            "alpha": pooled(f.alpha, st.sigma_alpha),
            "delta": pooled(f.delta, st.sigma_delta),
        },
        "assignment": assignment,
        "label": np.asarray(split.fret_labels, dtype=int),
        "E": np.asarray(sp.E, dtype=float),
        "S": np.asarray(sp.S, dtype=float),
        "model_selection": {
            "selected": str(sp.selected),
            "identifiable": bool(sp.identifiable),
            "criterion": "BIC = chi2 + k ln(n_obs)",
            "bic_shared": float(sp.bic_shared), "bic_species": float(sp.bic_species),
            "chi2_shared": float(sp.chi2_shared), "chi2_species": float(sp.chi2_species),
            "n_obs": int(sp.n_obs), "k_shared": int(sp.k_shared), "k_species": int(sp.k_species),
            "gamma_shared": float(sp.gamma_shared), "sigma_gamma_shared": float(sp.sigma_gamma_shared),
            "beta_shared": float(sp.beta_shared),
            "gamma_species": arr(sp.gamma_species), "sigma_gamma_species": arr(sp.sigma_gamma_species),
            "beta_species": float(sp.beta_species),
        },
    }


def species_factors(i_dd, i_da, i_aa, probabilities, *, factors=None, tau_d=None, tau_a=None,
                    line=None, sigma_model=0.01):
    """Shared versus species-specific gamma for given FRET populations.

    Parameters
    ----------
    probabilities : array_like
        ``(n_bursts, n_populations)`` assignment probabilities (0 rows for
        bursts outside the FRET class).
    factors : mapping, optional
        alpha, delta and backgrounds of the signals and the starting gamma and
        beta (``auto_calibrate``'s ``factors``).
    tau_d, tau_a, line : optional
        Donor and acceptor lifetimes (ns) and the static FRET line; without a
        donor lifetime the species model is not identifiable.

    Returns
    -------
    dict
        Per population ``n``, ``E``, ``S``, ``tau_d``, ``E_line``, ``tau_a``,
        ``gamma``, ``sigma_gamma``; ``beta``; the two fits and their BICs
        under ``model_selection``; per-burst ``E`` and ``S``.
    """
    np = _afret_np()
    p = np.asarray(probabilities, dtype=float)
    p = p.reshape(p.shape[0], -1)
    lt, le = _afret_line(line)
    vec = lambda v: [] if v is None else _afret_vec(v)
    r = _afret_species_factors(_afret_vec(i_dd), _afret_vec(i_da), _afret_vec(i_aa), _afret_vec(p),
                               int(p.shape[1]), _afret_factors_from(dict(factors or {})), vec(tau_d),
                               vec(tau_a), lt, le, float(sigma_model))
    arr = lambda v: np.asarray(v, dtype=float)
    return {"n": arr(r.n_eff), "E_pop": arr(r.E_pop), "S_pop": arr(r.S_pop), "tau_d": arr(r.tau_d),
            "E_line": arr(r.e_line), "tau_a": arr(r.tau_a), "gamma": arr(r.gamma),
            "sigma_gamma": arr(r.sigma_gamma), "beta": float(r.beta), "E": arr(r.E), "S": arr(r.S),
            "model_selection": {"selected": str(r.selected), "identifiable": bool(r.identifiable),
                                "bic_shared": float(r.bic_shared), "bic_species": float(r.bic_species),
                                "gamma_shared": float(r.gamma_shared),
                                "gamma_species": arr(r.gamma_species)}}
