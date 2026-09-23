# SPDX-License-Identifier: BSD-3-Clause
"""numpy transcription of chisurf's uncertainty, distance and bootstrap code (2026-09-23).

From `fret/accurate.py` (efficiency_uncertainty, distance_from_efficiency,
accurate_fret, _population_summary, _bootstrap_uncertainties) and the data
part of `fret/calibration.py:refine_calibration`. The bootstrap draws with
``numpy.random.default_rng(seed).integers(0, size, size)`` exactly as chisurf
did; `draw_like_chisurf` hands the same draws to tttrlib.
"""
import numpy as np

from . import calibrate as cal
from . import es as es_ref
from . import populations as pop


def efficiency_uncertainty(efficiency, f_dd, f_aa=None, *, gamma=1.0, sigma_gamma=0.0,
                           sigma_alpha=0.0, sigma_delta=0.0, sigma_statistical=None):
    e = np.asarray(efficiency, dtype=float)
    dd = np.asarray(f_dd, dtype=float)
    g = float(gamma) if gamma else 1.0
    om = 1.0 - e
    d_gamma = np.abs(e * om / g) * float(sigma_gamma)
    d_alpha = np.abs(om ** 2 / g) * float(sigma_alpha)
    if f_aa is None:
        d_delta = np.zeros_like(e)
    else:
        aa = np.asarray(f_aa, dtype=float)
        with np.errstate(divide="ignore", invalid="ignore"):
            ratio = np.where(dd != 0, aa / (g * dd), 0.0)
        d_delta = np.abs(om ** 2 * ratio) * float(sigma_delta)
    systematic = np.sqrt(d_gamma ** 2 + d_alpha ** 2 + d_delta ** 2)
    statistical = np.zeros_like(systematic) if sigma_statistical is None \
        else np.abs(np.asarray(sigma_statistical, dtype=float))
    return {"total": np.sqrt(systematic ** 2 + statistical ** 2), "systematic": systematic,
            "statistical": statistical, "terms": {"gamma": d_gamma, "alpha": d_alpha, "delta": d_delta}}


def distance_from_efficiency(efficiency, r0, *, sigma_efficiency=None, sigma_r0=0.0):
    e = np.asarray(efficiency, dtype=float)
    with np.errstate(divide="ignore", invalid="ignore"):
        valid = (e > 0.0) & (e < 1.0)
        r = np.where(valid, float(r0) * (1.0 / np.where(valid, e, 0.5) - 1.0) ** (1.0 / 6.0), np.nan)
    rel_r0 = (float(sigma_r0) / float(r0)) if r0 else 0.0
    if sigma_efficiency is None:
        rel_e = np.zeros_like(r)
    else:
        se = np.abs(np.asarray(sigma_efficiency, dtype=float))
        with np.errstate(divide="ignore", invalid="ignore"):
            rel_e = np.where(valid, se / (6.0 * e * (1.0 - e)), np.nan)
    return {"distance": r, "sigma": r * np.sqrt(rel_r0 ** 2 + rel_e ** 2)}


def _population_summary(e, s, tau_f, labels, factors, unc, line, sigma_systematic):
    out = []
    for u in np.unique(labels):
        m = labels == u
        n = int(np.count_nonzero(m & np.isfinite(e)))
        if n == 0:
            continue
        e_m = float(np.nanmean(e[m]))
        sem = float(np.nanstd(e[m]) / max(np.sqrt(n), 1.0))
        sys_err = float(np.nanmean(np.asarray(sigma_systematic, dtype=float)[m]))
        total = float(np.hypot(sys_err, sem))
        d = distance_from_efficiency(e_m, factors["r0"], sigma_efficiency=total, sigma_r0=unc["r0"])
        entry = {"label": int(u), "n": n, "E": e_m, "sigma_E": total, "sigma_E_statistical": sem,
                 "sigma_E_systematic": sys_err,
                 "S": float(np.nanmean(s[m])) if s is not None else float("nan"),
                 "distance": float(np.atleast_1d(d["distance"])[0]),
                 "sigma_distance": float(np.atleast_1d(d["sigma"])[0])}
        if tau_f is not None:
            t = np.asarray(tau_f, dtype=float)[m]
            t = t[np.isfinite(t)]
            entry["tau_f"] = float(np.mean(t)) if t.size else float("nan")
            if line is not None and np.isfinite(entry["tau_f"]):
                entry["E_line"] = float(line.efficiency_at(entry["tau_f"]))
                entry["deviation"] = entry["E"] - entry["E_line"]
        out.append(entry)
    return out


def accurate_fret(i_dd, i_da, i_aa=None, *, factors, uncertainties=None, tau_f=None, line=None,
                  labels=None):
    unc = {"gamma": 0.0, "alpha": 0.0, "delta": 0.0, "r0": 0.0}
    for key, value in (uncertainties or {}).items():
        unc[key] = float(value) if value is not None and np.isfinite(value) else 0.0
    f = factors
    es = es_ref.corrected_es(i_dd, i_da, i_aa, gamma=f["gamma"], alpha=f["alpha"], beta=f["beta"],
                             delta=f["delta"], bg_dd=f["bg_dd"], bg_da=f["bg_da"], bg_aa=f["bg_aa"])
    e = np.asarray(es["E"], dtype=float)
    f_dd = np.asarray(i_dd, dtype=float) - f["bg_dd"]
    f_aa = None if i_aa is None else np.asarray(i_aa, dtype=float) - f["bg_aa"]
    sigma = efficiency_uncertainty(e, f_dd, f_aa, gamma=f["gamma"], sigma_gamma=unc["gamma"],
                                   sigma_alpha=unc["alpha"], sigma_delta=unc["delta"])
    dist = distance_from_efficiency(e, f["r0"], sigma_efficiency=sigma["total"], sigma_r0=unc["r0"])
    deviation = None
    if line is not None and tau_f is not None:
        deviation = e - line.efficiency_at(tau_f)
    labels = np.zeros(e.shape, dtype=int) if labels is None else np.asarray(labels)
    pops = _population_summary(e, es["S"], tau_f, labels, f, unc, line, sigma["systematic"])
    return {"E": e, "S": es["S"], "fc": es["fc"], "sigma_E": sigma["total"],
            "sigma_E_systematic": sigma["systematic"], "distance": dist["distance"],
            "sigma_distance": dist["sigma"], "deviation": deviation, "populations": pops}


def bootstrap(calib, dd, da, aa, tau, line, split, *, n_bootstrap, seed, min_population, **_):
    out = {k: float("nan") for k in ("alpha", "delta", "gamma", "beta", "r0")}
    if not n_bootstrap or split is None:
        return out
    rng = np.random.default_rng(int(seed))
    got = {"alpha": [], "delta": [], "gamma": [], "beta": []}
    idx_d, idx_a, idx_f = (np.flatnonzero(split[k]) for k in ("donor_only", "acceptor_only", "fret"))
    for _ in range(int(n_bootstrap)):
        if idx_d.size:
            s = rng.integers(0, idx_d.size, idx_d.size)
            got["alpha"].append(pop.leakage_from_donor_only(dd[idx_d[s]], da[idx_d[s]],
                                                            bg_dd=calib.bg_dd, bg_da=calib.bg_da))
        if aa is not None and idx_a.size:
            s = rng.integers(0, idx_a.size, idx_a.size)
            got["delta"].append(pop.direct_excitation_from_acceptor_only(
                da[idx_a[s]], aa[idx_a[s]], dd[idx_a[s]], alpha=calib.alpha, bg_dd=calib.bg_dd,
                bg_da=calib.bg_da, bg_aa=calib.bg_aa))
        if idx_f.size:
            s = rng.integers(0, idx_f.size, idx_f.size)
            j = idx_f[s]
            labels = split["fret_labels"][j]
            if aa is not None and len(np.unique(labels)) >= 2:
                est = cal.global_es_correction(dd[j], da[j], aa[j], labels, alpha=calib.alpha,
                                               delta=calib.delta)
                if np.isfinite(est["gamma"]):
                    got["gamma"].append(est["gamma"])
                if np.isfinite(est["beta"]):
                    got["beta"].append(est["beta"])
            elif tau is not None and line is not None:
                lt = cal.gamma_from_lifetime(
                    dd[j], da[j], tau[j], line=line, i_aa=None if aa is None else aa[j],
                    alpha=calib.alpha, delta=calib.delta, bg_dd=calib.bg_dd, bg_da=calib.bg_da,
                    bg_aa=calib.bg_aa, labels=labels, min_population=min_population)
                if np.isfinite(lt["gamma"]):
                    got["gamma"].append(lt["gamma"])
    for key, values in got.items():
        if len(values) > 2:
            out[key] = float(np.std(np.asarray(values, dtype=float)))
    return out


def final_populations(calib, dd, da, aa, tau, line, unc, split):
    labels = np.where(split["fret"], split["fret_labels"], -1) if split is not None else None
    res = accurate_fret(dd, da, aa, factors=calib.as_dict(), uncertainties=unc, tau_f=tau,
                        line=line, labels=labels)
    return [p for p in res["populations"] if p["label"] != -1]


def draw_like_chisurf(seed):
    """``draw(size)`` for tttrlib's ``bootstrap_indices``: chisurf's own draws, in order."""
    rng = np.random.default_rng(int(seed))
    return lambda size: rng.integers(0, size, size)


def refine_gamma(i_dd, i_da, i_aa, labels, *, alpha, delta, prior=None, data_sigma=None,
                 n_bootstrap=60, seed=0):
    g, r, y = (np.asarray(v, dtype=float) for v in (i_dd, i_da, i_aa))
    labels = np.asarray(labels)
    est = cal.global_es_correction(g, r, y, labels, alpha=alpha, delta=delta)
    gamma_data = est["gamma"]
    gamma_prior = prior[0] if prior is not None else gamma_data
    if not np.isfinite(gamma_data):
        return {"gamma_data": gamma_data, "beta": est["beta"], "data_sigma": float("nan"),
                "gamma_prior": gamma_prior, "gamma_posterior": float("nan")}
    if data_sigma is None:
        rng = np.random.default_rng(seed)
        n = labels.size
        boot = []
        for _ in range(int(n_bootstrap)):
            idx = rng.integers(0, n, n)
            try:
                gb = cal.global_es_correction(g[idx], r[idx], y[idx], labels[idx], alpha=alpha,
                                              delta=delta)["gamma"]
                if np.isfinite(gb):
                    boot.append(gb)
            except Exception:
                continue
        data_sigma = float(np.std(boot)) if len(boot) > 2 else abs(gamma_data) * 0.1
    data_sigma = max(float(data_sigma), 1e-6)
    post = gamma_data
    if prior is not None:
        w_d, w_p = 1.0 / data_sigma ** 2, 1.0 / float(prior[1]) ** 2
        post = (gamma_data * w_d + gamma_prior * w_p) / (w_d + w_p)
    return {"gamma_data": gamma_data, "beta": est["beta"], "data_sigma": data_sigma,
            "gamma_prior": gamma_prior, "gamma_posterior": post}
