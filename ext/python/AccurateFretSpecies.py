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
