# SPDX-License-Identifier: BSD-3-Clause
"""numpy transcription of chisurf's `auto_calibrate` loop (2026-09-23).

Everything but the chisurf-side plumbing: the light-path payload parsing and
the static-line construction (the caller passes a `calibrate.Line`), and the
AutoCalibration dataclass (a dict here). `accurate_fret` and the bootstrap are
injected so the uncertainty port can extend this without copying it.
"""
import numpy as np

from . import calibrate as cal
from . import es as es_ref
from . import populations as pop


def _split_no_alex(e_cur, max_fret_populations, min_population):
    n = e_cur.shape
    return {"donor_only": np.zeros(n, dtype=bool), "acceptor_only": np.zeros(n, dtype=bool),
            "fret": np.ones(n, dtype=bool),
            "fret_labels": pop.split_fret_subpopulations(
                e_cur, max_populations=max_fret_populations, min_population=min_population),
            "thresholds": (0.0, 1.0), "method": "none", "components": {}}


def _estimate_alpha_delta(calib, dd, da, aa, split, messages):
    estimated = {"alpha": False, "delta": False}
    if np.any(split["donor_only"]):
        m = split["donor_only"]
        calib.alpha = pop.leakage_from_donor_only(dd[m], da[m], bg_dd=calib.bg_dd, bg_da=calib.bg_da)
        estimated["alpha"] = True
    else:
        messages.append("no donor-only population in the data")
    if aa is not None and np.any(split["acceptor_only"]):
        m = split["acceptor_only"]
        calib.delta = pop.direct_excitation_from_acceptor_only(
            da[m], aa[m], dd[m], alpha=calib.alpha, bg_dd=calib.bg_dd, bg_da=calib.bg_da,
            bg_aa=calib.bg_aa)
        estimated["delta"] = True
    else:
        messages.append("no acceptor-only population in the data")
    return estimated


def _estimate_gamma_beta_es(calib, dd, da, aa, split):
    if aa is None:
        return float("nan"), float("nan")
    m = split["fret"] & (split["fret_labels"] >= 0)
    if np.count_nonzero(m) < 2 or len(np.unique(split["fret_labels"][m])) < 2:
        return float("nan"), float("nan")
    try:
        est = cal.global_es_correction(dd[m], da[m], aa[m], split["fret_labels"][m],
                                       alpha=calib.alpha, delta=calib.delta)
    except ValueError:
        return float("nan"), float("nan")
    g = est["gamma"] if np.isfinite(est["gamma"]) and est["gamma"] > 0 else float("nan")
    b = est["beta"] if np.isfinite(est["beta"]) and est["beta"] > 0 else float("nan")
    return float(g), float(b)


def auto_calibrate(i_dd, i_da, i_aa=None, *, calib=None, tau_f=None, line=None,
                   gamma_source="auto", n_iterations=6, tolerance=1e-3, n_bootstrap=0, seed=0,
                   use_priors=True, assume_one_to_one=True, min_population=20,
                   max_fret_populations=3, donor_only_above=0.75, acceptor_only_below=0.25,
                   bootstrap=None, final=None, **bootstrap_kw):
    calib = calib if calib is not None else cal.Calib()
    messages = []
    dd, da = np.asarray(i_dd, dtype=float), np.asarray(i_da, dtype=float)
    aa = None if i_aa is None else np.asarray(i_aa, dtype=float)
    tau = None if tau_f is None else np.asarray(tau_f, dtype=float)
    split = None
    est = {"es": float("nan"), "lifetime": float("nan"), "prior": float("nan"),
           "posterior": float("nan")}
    previous = np.array([calib.alpha, calib.delta, calib.gamma, calib.beta])
    converged, iteration, it_msgs, estimated = False, 0, [], {}
    for iteration in range(1, int(n_iterations) + 1):
        it_msgs = []
        es = es_ref.corrected_es(dd, da, aa, gamma=calib.gamma, alpha=calib.alpha,
                                 beta=calib.beta, delta=calib.delta, bg_dd=calib.bg_dd,
                                 bg_da=calib.bg_da, bg_aa=calib.bg_aa)
        e_cur = np.asarray(es["E"], dtype=float)
        if aa is None:
            if iteration == 1:
                messages.append("no acceptor-excitation channel: every burst is taken to be "
                                "doubly labelled — singly labelled bursts left in the data bias "
                                "gamma, so gate them out first")
            split = _split_no_alex(e_cur, max_fret_populations, min_population)
        else:
            split = pop.classify_es_populations(
                es["S"], e_cur, donor_only_above=donor_only_above,
                acceptor_only_below=acceptor_only_below,
                max_fret_populations=max_fret_populations, min_population=min_population)
        estimated = _estimate_alpha_delta(calib, dd, da, aa, split, it_msgs)
        gamma_es, beta_es = _estimate_gamma_beta_es(calib, dd, da, aa, split)
        est["es"] = gamma_es
        if tau is not None and line is not None:
            lt = cal.gamma_from_lifetime(
                dd, da, tau, line=line, i_aa=aa, alpha=calib.alpha, delta=calib.delta,
                bg_dd=calib.bg_dd, bg_da=calib.bg_da, bg_aa=calib.bg_aa,
                labels=np.where(split["fret"], split["fret_labels"], -1),
                min_population=min_population)
            est["lifetime"], est["lifetime_sigma"] = lt["gamma"], lt["sigma"]
        gamma = cal._select_gamma(gamma_source, est, it_msgs)
        estimated["gamma"] = bool(np.isfinite(gamma) and gamma > 0)
        if estimated["gamma"]:
            calib.gamma = float(np.clip(gamma, 0.01, 100.0))
        if np.isfinite(beta_es) and beta_es > 0:
            calib.beta = float(beta_es)
        elif aa is not None and assume_one_to_one and np.any(split["fret"]):
            m = split["fret"]
            calib.beta = cal.beta_from_stoichiometry(
                dd[m], da[m], aa[m], gamma=calib.gamma, alpha=calib.alpha, delta=calib.delta,
                bg_dd=calib.bg_dd, bg_da=calib.bg_da, bg_aa=calib.bg_aa)
            it_msgs.append("only one FRET population: beta defined by centring it at S = 0.5 "
                           "(1:1 labelling assumed)")
        current = np.array([calib.alpha, calib.delta, calib.gamma, calib.beta])
        if np.max(np.abs(current - previous)) < float(tolerance):
            converged = True
            previous = current
            break
        previous = current
    messages.extend(it_msgs)
    est["data"] = float(calib.gamma)
    boot = bootstrap or cal.bootstrap
    unc = boot(calib, dd, da, aa, tau, line, split, n_bootstrap=n_bootstrap, seed=seed,
               min_population=min_population, **bootstrap_kw)
    if not np.isfinite(unc.get("gamma", float("nan"))):
        unc["gamma"] = float(est.get("lifetime_sigma", float("nan")))
    if use_priors:
        post = cal._combine(calib, unc, estimated, messages)
        unc.update({k: v for k, v in post["sigma"].items() if v is not None})
        est["prior"] = post["prior"].get("gamma", float("nan"))
    est["posterior"] = float(calib.gamma)
    out = {"factors": calib.as_dict(), "uncertainties": unc, "split": split,
           "gamma_estimates": est, "estimated": estimated, "iterations": iteration,
           "converged": converged, "messages": messages, "populations": []}
    if final is not None:
        out["populations"] = final(calib, dd, da, aa, tau, line, unc, split)
    return out
