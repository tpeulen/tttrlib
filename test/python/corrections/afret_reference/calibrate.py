# SPDX-License-Identifier: BSD-3-Clause
"""numpy transcription of chisurf's auto_calibrate and its estimators (2026-09-23).

From `fret/accurate.py` (beta_from_stoichiometry, gamma_from_lifetime,
auto_calibrate, _estimate_alpha_delta, _estimate_gamma_beta_es, _select_gamma,
_combine_with_optics_priors) and `fret/calibration.py`
(global_es_correction). `Calib` stands in for CalibrationParameters: the same
bounds, clamped on write as its bounded FittingParameters are, and priors as
``(mu, sigma)``. The bootstrap is a hook (`bootstrap`) filled in by
`uncertainty.py`.
"""
import numpy as np

from . import es as es_ref
from . import populations as pop

BOUNDS = {"gamma": (0.05, 20.0), "alpha": (0.0, 1.0), "beta": (0.01, 100.0), "delta": (0.0, 1.0),
          "bg_dd": (0.0, 1e6), "bg_da": (0.0, 1e6), "bg_aa": (0.0, 1e6), "r0": (1.0, 200.0)}


class Calib:
    def __init__(self, priors=None, **values):
        self.priors = dict(priors or {})
        defaults = dict(gamma=1.0, alpha=0.0, beta=1.0, delta=0.0, bg_dd=0.0, bg_da=0.0,
                        bg_aa=0.0, r0=52.0)
        defaults.update(values)
        for k, v in defaults.items():
            setattr(self, k, v)

    def __setattr__(self, key, value):
        if key in BOUNDS:
            lo, hi = BOUNDS[key]
            value = min(hi, max(lo, float(value)))
        object.__setattr__(self, key, value)

    def as_dict(self):
        return {k: getattr(self, k) for k in BOUNDS}


class Line:
    def __init__(self, tau_f, efficiency):
        order = np.argsort(np.asarray(tau_f, dtype=float), kind="stable")
        self.tau_f = np.asarray(tau_f, dtype=float)[order]
        self.efficiency = np.asarray(efficiency, dtype=float)[order]

    def efficiency_at(self, tau_f):
        t = np.asarray(tau_f, dtype=float)
        return np.where(np.isfinite(t), np.interp(t, self.tau_f, self.efficiency), np.nan)


def global_es_correction(i_dd, i_da, i_aa, labels, *, alpha=0.0, delta=0.0):
    g, r, y = (np.asarray(v, dtype=float) for v in (i_dd, i_da, i_aa))
    labels = np.asarray(labels)
    f_da = r - alpha * g - delta * y
    gf = g + f_da
    with np.errstate(divide="ignore", invalid="ignore"):
        e_pr = np.where(gf != 0, f_da / gf, 0.0)
        s_pr = np.where((gf + y) != 0, gf / (gf + y), 0.0)
    uniq = list(np.unique(labels))
    if len(uniq) < 2:
        raise ValueError("global_es_correction needs >= 2 populations")
    e_mean = np.array([e_pr[labels == u].mean() for u in uniq])
    inv_s = np.array([1.0 / s_pr[labels == u].mean() for u in uniq])
    counts = np.array([np.count_nonzero(labels == u) for u in uniq], dtype=float)
    sigma, omega = np.polyfit(e_mean, inv_s, 1, w=np.sqrt(counts))
    denom = omega + sigma - 1.0
    gamma = (omega - 1.0) / denom if abs(denom) > 1e-12 else float("nan")
    return {"gamma": float(gamma), "beta": float(denom), "Omega": float(omega), "Sigma": float(sigma)}


def beta_from_stoichiometry(i_dd, i_da, i_aa, *, gamma, alpha=0.0, delta=0.0, bg_dd=0.0,
                            bg_da=0.0, bg_aa=0.0, target=0.5):
    f_dd = np.asarray(i_dd, dtype=float) - bg_dd
    f_aa = np.asarray(i_aa, dtype=float) - bg_aa
    f_da = np.asarray(i_da, dtype=float) - bg_da - alpha * f_dd - delta * f_aa
    num = float(np.mean(gamma * f_dd + f_da))
    den = float(np.mean(f_aa))
    t = float(np.clip(target, 1e-6, 1.0 - 1e-6))
    if num <= 0 or den <= 0:
        return 1.0
    return float(den / (num * (1.0 / t - 1.0)))


def gamma_from_lifetime(i_dd, i_da, tau_f, *, line, i_aa=None, alpha=0.0, delta=0.0, bg_dd=0.0,
                        bg_da=0.0, bg_aa=0.0, labels=None, min_population=20,
                        efficiency_window=(0.05, 0.95)):
    f_dd = np.asarray(i_dd, dtype=float) - bg_dd
    f_aa = None if i_aa is None else np.asarray(i_aa, dtype=float) - bg_aa
    f_da = np.asarray(i_da, dtype=float) - bg_da - alpha * f_dd
    if f_aa is not None and delta:
        f_da = f_da - delta * f_aa
    t = np.asarray(tau_f, dtype=float)
    labels = np.zeros(f_dd.shape, dtype=int) if labels is None else np.asarray(labels)
    entries = []
    for u in np.unique(labels):
        m = (labels == u) & np.isfinite(t) & np.isfinite(f_dd) & np.isfinite(f_da)
        n = int(np.count_nonzero(m))
        if n < min_population:
            continue
        tau_mean = float(np.mean(t[m]))
        e_line = float(line.efficiency_at(tau_mean))
        dd, da = float(np.mean(f_dd[m])), float(np.mean(f_da[m]))
        if not (efficiency_window[0] <= e_line <= efficiency_window[1]) or dd <= 0 or da <= 0:
            continue
        entries.append({"label": int(u), "n": n, "tau_f": tau_mean, "E_line": e_line,
                        "gamma": float((da / dd) * (1.0 - e_line) / e_line)})
    if not entries:
        return {"gamma": float("nan"), "sigma": float("nan"), "populations": []}
    w = np.array([p["n"] for p in entries], dtype=float)
    v = np.array([p["gamma"] for p in entries], dtype=float)
    gamma = float(np.sum(w * v) / np.sum(w))
    sigma = float(np.sqrt(float(np.sum(w * (v - gamma) ** 2) / np.sum(w)) / v.size)) \
        if v.size > 1 else float("nan")
    return {"gamma": gamma, "sigma": sigma, "populations": entries}


def _select_gamma(source, est, messages):
    es, lt = est.get("es", float("nan")), est.get("lifetime", float("nan"))
    if source == "es":
        return es
    if source == "lifetime":
        return lt
    if source == "combined":
        vals = [v for v in (es, lt) if np.isfinite(v)]
        return float(np.mean(vals)) if vals else float("nan")
    if np.isfinite(es):
        return es
    if np.isfinite(lt):
        messages.append("fewer than two FRET populations: gamma taken from the donor "
                        "lifetime and the static FRET line")
        return lt
    messages.append("gamma could not be determined from the data; prior value kept")
    return float("nan")


def _combine(calib, data_sigma, estimated, messages):
    out = {"prior": {}, "sigma": {}}
    for name in ("gamma", "alpha", "delta"):
        prior = calib.priors.get(name)
        sigma_d = float(data_sigma.get(name, float("nan")))
        if prior is None:
            out["sigma"][name] = sigma_d
            if not estimated.get(name):
                messages.append(f"{name} was neither identified by the data nor constrained by "
                                f"the light path; it keeps its current value")
            continue
        mu, sigma_p = prior
        out["prior"][name] = mu
        if not estimated.get(name):
            setattr(calib, name, mu)
            out["sigma"][name] = sigma_p
            messages.append(f"{name} not identifiable from the data; the light-path value "
                            f"{mu:.4f} ± {sigma_p:.4f} is used")
            continue
        value = float(getattr(calib, name))
        if not np.isfinite(sigma_d) or sigma_d <= 0:
            sigma_d = max(abs(value) * 0.1, 1e-6)
        w_d, w_p = 1.0 / sigma_d ** 2, 1.0 / max(sigma_p, 1e-9) ** 2
        setattr(calib, name, float((value * w_d + mu * w_p) / (w_d + w_p)))
        out["sigma"][name] = float(np.sqrt(1.0 / (w_d + w_p)))
        messages.append(f"{name}: data {value:.4f} ± {sigma_d:.4f} combined with the light-path "
                        f"prior {mu:.4f} ± {sigma_p:.4f} → {float(getattr(calib, name)):.4f}")
    return out


def bootstrap(calib, dd, da, aa, tau, line, split, **kw):
    """Replaced by uncertainty.bootstrap once the uncertainty port exists."""
    return {k: float("nan") for k in ("alpha", "delta", "gamma", "beta", "r0")}
