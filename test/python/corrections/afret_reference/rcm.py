# SPDX-License-Identifier: BSD-3-Clause
"""numpy transcription of chisurf `fret/calibration.py:rcm_from_dye_solutions` (2026-09-23)."""
import numpy as np


def rcm_from_dye_solutions(
    donor_sample_rates,
    acceptor_sample_rates,
    absorbance_ratio: float,
    detector_assignment,
    anisotropy=(0.0, 0.0),
):
    """Routing/detection-correction matrix (RCM) from dye-solution measurements.

    Port of Fretica ``FRCMCalibrationFromDyeSolutions``.  From per-channel count
    rates of a **donor-only** and an **acceptor-only** dye solution, the relative
    absorbance ``AbsorbanceA / AbsorbanceD`` and the detector layout, it solves
    for the matrix that corrects measured channel rates for detection
    efficiencies and cross-talk between the spectral (and, with a polarising
    beam-splitter, polarisation) channels — a per-setup calibration that removes
    the need to hand-enter correction factors.

    Parameters
    ----------
    donor_sample_rates, acceptor_sample_rates : array_like
        Background-corrected count rate in every channel for the donor-only and
        acceptor-only dye solution (length = number of channels).
    absorbance_ratio : float
        ``AbsorbanceA / AbsorbanceD`` of the two calibration solutions (relative
        excitation/concentration).
    detector_assignment : sequence of (str, str)
        Per channel ``(species, polarisation)`` with ``species in {"A", "D"}``
        and ``polarisation in {"P", "S"}`` (polarisation ignored for a 2-channel
        setup).  Must have an equal number of A and D channels (1 or 2 each).
    anisotropy : (float, float)
        ``(r_donor, r_acceptor)`` steady-state anisotropies (used only for a
        4-channel polarising-beam-splitter setup; ``(0, 0)`` for a 50/50 split).

    Returns
    -------
    numpy.ndarray
        The ``nchtot x nchtot`` correction matrix (identity on unused channels,
        normalised so its first ordered element is 1).
    """
    donor_sample_rates = np.asarray(donor_sample_rates, dtype=float)
    acceptor_sample_rates = np.asarray(acceptor_sample_rates, dtype=float)
    nchtot = len(detector_assignment)
    if donor_sample_rates.size != nchtot or acceptor_sample_rates.size != nchtot:
        raise ValueError("rate vectors must match the number of detector channels")

    # the polarisation entry of each assignment is implicit in the channel order
    # below (a_idx/d_idx) together with `anisotropy`, so only the species is read
    species = [d[0] for d in detector_assignment]
    nchA = species.count("A")
    nchD = species.count("D")
    if nchA != nchD or nchA == 0 or nchA > 2:
        raise ValueError("need an equal number of A and D channels (1 or 2 each)")
    nch = nchA + nchD
    alpha = float(absorbance_ratio)
    rd, ra = anisotropy

    a_idx = [i for i, s in enumerate(species) if s == "A"]
    d_idx = [i for i, s in enumerate(species) if s == "D"]

    if nch == 2:
        order = [a_idx[0], d_idx[0]]
        pa = pd = 0.0
    else:

        def _find(sp, pl):
            return [i for i, d in enumerate(detector_assignment) if d == (sp, pl)]

        polarized = all(
            len(_find(sp, pl)) == 1 for sp, pl in [("A", "P"), ("D", "P"), ("A", "S"), ("D", "S")]
        )
        if polarized:
            order = [_find("A", "P")[0], _find("D", "P")[0], _find("A", "S")[0], _find("D", "S")[0]]
            pa = (3.0 * ra) / (2.0 + ra)
            pd = (3.0 * rd) / (2.0 + rd)
        else:  # 50/50 beam splitter, polarisation not resolved
            order = [a_idx[0], d_idx[0], a_idx[1], d_idx[1]]
            pa = pd = 0.0

    nd = donor_sample_rates[order]
    na = acceptor_sample_rates[order]

    if nch == 2:
        amat = np.array([[na[0], alpha * nd[0]], [na[1], alpha * nd[1]]], dtype=float)
    else:
        amat = np.array(
            [
                [na[0] / (1 + pa), alpha * nd[0] / (1 + pd), 0.0, 0.0],
                [na[1] / (1 + pa), alpha * nd[1] / (1 + pd), 0.0, 0.0],
                [0.0, 0.0, na[2] / (1 - pa), alpha * nd[2] / (1 - pd)],
                [0.0, 0.0, na[3] / (1 - pa), alpha * nd[3] / (1 - pd)],
            ],
            dtype=float,
        )

    rcm_sub = np.linalg.inv(amat)
    rcm_sub /= rcm_sub[0, 0]

    # Scatter the nch x nch block back into full channel space (identity elsewhere).
    perm = np.zeros((nch, nchtot), dtype=float)
    for i, o in enumerate(order):
        perm[i, o] = 1.0
    rcm = perm.T @ rcm_sub @ perm
    for i in range(nchtot):
        if i not in order:
            rcm[i, i] = 1.0
    return rcm
