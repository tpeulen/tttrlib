# SPDX-License-Identifier: BSD-3-Clause
"""numpy transcription of chisurf's population gating (2026-09-23).

From `core/fluorescence/fret/accurate.py` (gaussian_mixture_1d, _em_1d,
_best_mixture, classify_es_populations, split_fret_subpopulations) and
`fret/calibration.py` (leakage_from_donor_only,
direct_excitation_from_acceptor_only). `_em_1d` delegated to
`chisurf.core.ml.GaussianMixture(covariance_type="spherical")`; its EM loop
is inlined here with the same numpy operations, including the width update
`((resp * diff) ** 2).sum(axis=0) / Nk`.
"""
import numpy as np

_TINY = np.finfo(float).tiny


def _log_proba(X, means, covars, weights):
    covars = np.maximum(np.repeat(np.asarray(covars)[:, None], 1, axis=1), _TINY)
    lp = -0.5 * (1 * np.log(2 * np.pi) + np.log(covars).sum(axis=-1)
                 + ((X[:, None, :] - means) ** 2 / covars).sum(axis=-1))
    return lp + np.log(np.maximum(weights, _TINY))[None, :]


def _row_lse(lp):
    m = lp.max(axis=1)
    dead = ~np.isfinite(m)
    safe = np.where(dead, 0.0, m)
    total = np.exp(lp - safe[:, None]).sum(axis=1)
    with np.errstate(divide="ignore"):
        out = np.log(total) + safe
    return np.where(dead, -np.inf, out)


def _resp(lp):
    m = lp.max(axis=1, keepdims=True)
    dead = ~np.isfinite(m)
    safe = np.where(dead, 0.0, m)
    r = np.where(dead, 1.0, np.exp(lp - safe))
    d = r.sum(axis=1, keepdims=True)
    return r / np.where(d <= 0, 1.0, d)


def _em_1d(x, means, *, n_iterations, tolerance, sigma_floor):
    k = int(np.size(means))
    n = int(x.size)
    if k > n:
        raise ValueError("more components than samples")
    X = x[:, None]
    spread = float(np.std(x)) or 1.0
    sigma_start = max(spread / max(k, 1), sigma_floor)
    mu = np.asarray(means, dtype=float).reshape(k, 1).copy()
    cov = np.full(k, sigma_start ** 2)
    w = np.full(k, 1.0 / k)
    w = w / w.sum()
    reg = float(sigma_floor) ** 2
    prev = -np.inf
    n_iter = n_iterations
    for it in range(1, n_iterations + 1):
        lp = _log_proba(X, mu, cov, w)
        lb = float(_row_lse(lp).sum())
        resp = _resp(lp)
        nk = np.maximum(resp.sum(axis=0), _TINY)
        w = nk / n
        mu = (resp.T @ X) / nk[:, None]
        diff = X[:, None, :] - mu[None]
        s2 = (((resp[:, :, None] * diff) ** 2).sum(axis=0)).mean(axis=1) / nk
        cov = np.maximum(s2, _TINY) + reg
        if lb - prev < tolerance:
            n_iter = it
            break
        prev = lb
    lp = _log_proba(X, mu, cov, w)
    ll = float(_row_lse(lp).sum())
    resp = _resp(lp)
    fm = mu.ravel()
    sig = np.maximum(np.sqrt(np.maximum(cov, 0.0)), sigma_floor)
    order = np.argsort(fm, kind="stable")
    return {
        "weights": w[order], "means": fm[order], "sigmas": sig[order],
        "responsibilities": resp[:, order], "labels": np.argmax(resp[:, order], axis=1),
        "log_likelihood": ll, "bic": float((3 * k - 1) * np.log(max(n, 2)) - 2.0 * ll),
        "n_iter": n_iter,
    }


def gaussian_mixture_1d(x, n_components, *, n_iterations=300, tolerance=1e-7,
                        sigma_floor=1e-3, init="auto"):
    x = np.asarray(x, dtype=float).ravel()
    x = x[np.isfinite(x)]
    k = max(1, int(n_components))
    if x.size == 0:
        raise ValueError("needs at least one finite sample")
    starts = []
    if init in ("auto", "quantile"):
        starts.append(np.quantile(x, (np.arange(k) + 0.5) / k))
    if init in ("auto", "range") and k > 1:
        lo, hi = float(np.min(x)), float(np.max(x))
        starts.append(np.linspace(lo, hi, k) if hi > lo else np.full(k, lo))
    if not starts:
        starts.append(np.quantile(x, (np.arange(k) + 0.5) / k))
    best = None
    for start in starts:
        fit = _em_1d(x, start, n_iterations=n_iterations, tolerance=tolerance,
                     sigma_floor=sigma_floor)
        if best is None or fit["log_likelihood"] > best["log_likelihood"]:
            best = fit
    return best


def best_mixture(x, *, max_components=4, min_weight=0.02):
    x = np.asarray(x, dtype=float).ravel()
    x = x[np.isfinite(x)]
    best = None
    bics = {}
    for k in range(1, int(max_components) + 1):
        if x.size < 5 * k:
            break
        try:
            fit = gaussian_mixture_1d(x, k)
        except ValueError:
            break
        bics[k] = fit["bic"]
        if np.min(fit["weights"]) < min_weight and k > 1:
            continue
        if best is None or fit["bic"] < best["bic"]:
            best = fit
    if best is None:
        best = gaussian_mixture_1d(x, 1)
    best["bic_by_k"] = bics
    return best


def split_fret_subpopulations(efficiency, *, max_populations=3, min_population=20,
                              min_separation=0.05, min_fraction=0.1):
    e = np.asarray(efficiency, dtype=float).ravel()
    labels = np.zeros(e.size, dtype=int)
    finite = np.isfinite(e)
    if np.count_nonzero(finite) < 2 * min_population or max_populations < 2:
        return labels
    fit = best_mixture(e[finite], max_components=int(max_populations))
    lab = np.zeros(e.size, dtype=int)
    lab[finite] = fit["labels"]
    means = fit["means"]
    n_finite = int(np.count_nonzero(finite))
    floor = max(int(min_population), int(np.ceil(float(min_fraction) * n_finite)))
    keep = []
    for i in range(means.size):
        if int(np.count_nonzero(lab[finite] == i)) < floor:
            continue
        if keep and abs(means[i] - means[keep[-1]]) < min_separation:
            continue
        keep.append(i)
    if len(keep) < 2:
        return labels
    remap = {c: j for j, c in enumerate(keep)}
    out = np.zeros(e.size, dtype=int)
    for i in range(means.size):
        target = remap.get(i)
        if target is None:
            target = int(np.argmin([abs(means[i] - means[c]) for c in keep]))
        out[lab == i] = target
    return out


def classify_es_populations(stoichiometry, efficiency=None, *, donor_only_above=0.75,
                            acceptor_only_below=0.25, max_components=4, max_fret_populations=3,
                            min_population=20, reference_sigma=2.0, method="auto"):
    s = np.asarray(stoichiometry, dtype=float).ravel()
    finite = np.isfinite(s)
    lo, hi = float(acceptor_only_below), float(donor_only_above)
    used, components = "threshold", {}
    donor_core = acceptor_core = None
    if method != "threshold" and np.count_nonzero(finite) >= 5 * 3:
        try:
            fit = best_mixture(s[finite], max_components=max_components)
            means, sigmas = fit["means"], fit["sigmas"]
            klass = np.where(means >= hi, 1, np.where(means <= lo, -1, 0))
            if np.any(klass == 0):
                fret_means = means[klass == 0]
                if np.any(klass == -1):
                    index = int(np.argmax(np.where(klass == -1, means, -np.inf)))
                    lo = float(0.5 * (means[index] + np.min(fret_means)))
                    if reference_sigma > 0:
                        acceptor_core = float(means[index] + reference_sigma * sigmas[index])
                if np.any(klass == 1):
                    index = int(np.argmin(np.where(klass == 1, means, np.inf)))
                    hi = float(0.5 * (np.max(fret_means) + means[index]))
                    if reference_sigma > 0:
                        donor_core = float(means[index] - reference_sigma * sigmas[index])
                used = "mixture"
                components = {"means": means.tolist(), "weights": fit["weights"].tolist(),
                              "sigmas": sigmas.tolist(), "bic_by_k": fit.get("bic_by_k", {})}
        except Exception:
            used = "threshold"
    donor_only = finite & (s > max(hi, donor_core if donor_core is not None else hi))
    acceptor_only = finite & (s < min(lo, acceptor_core if acceptor_core is not None else lo))
    fret = finite & ~(s > hi) & ~(s < lo)
    if np.count_nonzero(donor_only) < min_population:
        donor_only = np.zeros_like(donor_only)
    if np.count_nonzero(acceptor_only) < min_population:
        acceptor_only = np.zeros_like(acceptor_only)
    fret_labels = np.full(s.shape, -1, dtype=int)
    if efficiency is not None and np.count_nonzero(fret) >= min_population:
        e = np.asarray(efficiency, dtype=float).ravel()
        fret_labels[fret] = split_fret_subpopulations(
            e[fret], max_populations=max_fret_populations, min_population=min_population)
    elif np.any(fret):
        fret_labels[fret] = 0
    return {"donor_only": donor_only, "acceptor_only": acceptor_only, "fret": fret,
            "fret_labels": fret_labels, "thresholds": (lo, hi), "method": used,
            "components": components}


def leakage_from_donor_only(i_dd, i_da, *, bg_dd=0.0, bg_da=0.0):
    f_dd = np.asarray(i_dd, dtype=float) - bg_dd
    f_da = np.asarray(i_da, dtype=float) - bg_da
    denom = float(np.mean(f_dd))
    return float(np.mean(f_da) / denom) if denom != 0 else 0.0


def direct_excitation_from_acceptor_only(i_da, i_aa, i_dd=None, *, alpha=0.0, bg_dd=0.0,
                                         bg_da=0.0, bg_aa=0.0):
    f_da = np.asarray(i_da, dtype=float) - bg_da
    f_aa = np.asarray(i_aa, dtype=float) - bg_aa
    if i_dd is not None and alpha:
        f_da = f_da - alpha * (np.asarray(i_dd, dtype=float) - bg_dd)
    denom = float(np.mean(f_aa))
    return float(np.mean(f_da) / denom) if denom != 0 else 0.0
