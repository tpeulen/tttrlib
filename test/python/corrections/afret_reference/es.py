# SPDX-License-Identifier: BSD-3-Clause
"""numpy transcription of chisurf `core/fluorescence/burst/es.py` (2026-09-23).

The A/B reference for tttrlib's AccurateFret E/S kernels. Transcribed rather
than imported so the comparison survives the removal of chisurf's copy;
`test_accurate_fret_es.py` checks it against chisurf while chisurf has it.
The three-cube step is inlined with the engine's guard convention (E = 0
where F_da + gamma F_dd <= 0), which is what chisurf forwarded to.
The stable (NNLS) un-mixing uses scipy.optimize.nnls on the augmented
ridge system, the construction chisurf's invert_mixing used.
"""
import numpy as np


def _three_cube(f_dd, f_da_raw, f_aa, alpha, delta, gamma):
    fc = f_da_raw - alpha * f_dd - delta * f_aa
    denom = fc + gamma * f_dd
    with np.errstate(divide="ignore", invalid="ignore"):
        e = np.where(denom > 0, fc / denom, 0.0)
    return fc, e


def apparent_es(i_dd, i_da, i_aa=None):
    a = np.asarray(i_dd, dtype=float)
    b = np.asarray(i_da, dtype=float)
    tot = a + b
    with np.errstate(divide="ignore", invalid="ignore"):
        e = np.where(tot != 0, b / tot, 0.0)
    s = None
    if i_aa is not None:
        c = np.asarray(i_aa, dtype=float)
        denom = tot + c
        with np.errstate(divide="ignore", invalid="ignore"):
            s = np.where(denom != 0, tot / denom, 0.0)
    return {"E": e, "S": s}


def corrected_es(i_dd, i_da, i_aa=None, *, gamma=1.0, alpha=0.0, beta=1.0, delta=0.0,
                 bg_dd=0.0, bg_da=0.0, bg_aa=0.0):
    a = np.asarray(i_dd, dtype=float)
    b = np.asarray(i_da, dtype=float)
    f_dd = a - bg_dd
    f_aa = np.asarray(i_aa, dtype=float) - bg_aa if i_aa is not None else np.zeros_like(f_dd)
    fc, e = _three_cube(f_dd, b - bg_da, f_aa, alpha, delta, gamma)
    s = None
    if i_aa is not None:
        num = gamma * f_dd + fc
        denom = num + f_aa / beta
        with np.errstate(divide="ignore", invalid="ignore"):
            s = np.where(denom != 0, num / denom, 0.0)
    return {"E": e, "S": s, "fc": fc}


def corrected_es_matrix(intensity, gamma, alpha, delta=None, background=None, pairs=None):
    inten = np.asarray(intensity, dtype=float)
    n = inten.shape[0]
    gamma = np.asarray(gamma, dtype=float)
    alpha = np.asarray(alpha, dtype=float)
    delta = np.zeros((n, n)) if delta is None else np.asarray(delta, dtype=float)
    background = np.zeros((n, n)) if background is None else np.asarray(background, dtype=float)
    if pairs is None:
        pairs = [(i, j) for i in range(n) for j in range(i + 1, n)]
    donors = {}
    for i, j in pairs:
        donors.setdefault(i, []).append(j)
    out = {}
    for i, acceptors in donors.items():
        f_ii = inten[i, i] - background[i, i]
        fc = {}
        for j in acceptors:
            fc[j] = ((inten[i, j] - background[i, j]) - float(alpha[i, j]) * f_ii
                     - float(delta[i, j]) * (inten[j, j] - background[j, j]))
        budget = f_ii + sum(fc[j] / float(gamma[i, j]) for j in acceptors)
        for j in acceptors:
            with np.errstate(divide="ignore", invalid="ignore"):
                e = np.where(budget != 0, (fc[j] / float(gamma[i, j])) / budget, 0.0)
            out[(i, j)] = {"E": e, "fc": fc[j]}
    return out


def _nnls_unmix(emis, measured, ridge):
    from scipy.optimize import nnls

    n_src, n_det = emis.shape
    a = emis.T
    if ridge > 0:
        a = np.vstack([a, np.sqrt(ridge) * np.eye(n_src)])
    y = measured.reshape(n_det, -1)
    out = np.empty((n_src, y.shape[1]))
    for c in range(y.shape[1]):
        target = np.concatenate([y[:, c], np.zeros(a.shape[0] - n_det)])
        out[:, c] = nnls(a, target)[0]
    return out.reshape((n_src,) + measured.shape[1:])


def corrected_es_general(intensity, excitation, emission, *, background=None, pairs=None,
                         unmix="naive", ridge=0.0):
    inten = np.asarray(intensity, dtype=float)
    exc = np.asarray(excitation, dtype=float)
    emis = np.asarray(emission, dtype=float)
    if background is not None:
        inten = inten - np.asarray(background, dtype=float)[(...,) + (None,) * (inten.ndim - 2)]
    n_laser, n_det = inten.shape[0], inten.shape[1]
    n_chrom = emis.shape[0]
    method = str(unmix).lower()
    if method in ("naive", "pinv", "linear"):
        if ridge and ridge > 0:
            gram = emis @ emis.T + float(ridge) * np.eye(n_chrom)
            unmix_mat = np.linalg.solve(gram, emis).T
        else:
            unmix_mat = np.linalg.pinv(emis)
        flat = inten.reshape(n_laser, n_det, -1)
        e_emit = np.einsum("lmb,mk->lkb", flat, unmix_mat)
        e_emit = e_emit.reshape((n_laser, n_chrom) + inten.shape[2:])
    elif method in ("stable", "nnls", "nonneg"):
        e_emit = np.stack([_nnls_unmix(emis, inten[i], float(ridge)) for i in range(n_laser)], axis=0)
    else:
        raise ValueError(f"unmix must be 'naive' or 'stable' (got {unmix!r})")
    if pairs is None:
        pairs = [(i, j) for i in range(n_laser) for j in range(i + 1, n_chrom)]
    donors = {}
    for i, j in pairs:
        donors.setdefault(i, []).append(j)
    out = {}
    for i, acceptors in donors.items():
        e_donor = e_emit[i, i]
        fc = {}
        for j in acceptors:
            x_rel = float(exc[i, j]) / float(exc[j, j]) if exc[j, j] != 0 else 0.0
            fc[j] = e_emit[i, j] - x_rel * e_emit[j, j]
        budget = e_donor + sum(fc[j] for j in acceptors)
        for j in acceptors:
            with np.errstate(divide="ignore", invalid="ignore"):
                e = np.where(budget != 0, fc[j] / budget, 0.0)
            out[(i, j)] = {"E": e, "fc": fc[j]}
    return out
