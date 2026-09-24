# SPDX-License-Identifier: BSD-3-Clause
# Burst-table channel conventions and the calibration report, shared by every
# accurate-FRET caller (ChiSurf, ndXplorer). Appended after AccurateFretCalibrate.py.
import re as _afret_re

#: Column-name fragments (lower case) identifying each channel role. Matched in
#: order, exact match first, so a more specific name wins over a substring.
BURST_COLUMN_HINTS = {
    "i_dd": ("i_dd", "i11", "green count rate", "f_dexc_dem", "sg", "number of photons (green)",
             "ngreen", "n green", "donor donor"),
    "i_da": ("i_da", "i12", "red count rate", "f_dexc_aem", "sr", "number of photons (red)",
             "nred", "n red", "donor acceptor"),
    "i_aa": ("i_aa", "i22", "delayed yellow", "yellow count rate", "f_aexc_aem", "sy",
             "number of photons (yellow)", "nyellow", "n yellow", "acceptor acceptor"),
    "tau_f": ("tau_f", "tau (green)", "taud(a)", "lifetime green", "green lifetime",
              "donor lifetime", "tau green"),
}

#: Name fragments that mark a detector or an excitation window donor- or acceptor-side.
BURST_DETECTOR_ROLE_WORDS = {
    "donor": ("green", "donor", "prompt", "d"),
    "acceptor": ("red", "acceptor", "delay", "delayed", "yellow", "a"),
}

_AFRET_GATED = _afret_re.compile(
    r"^s\s+(?P<middle>.+?)\s+\((?P<unit>khz|photons)\)\s*\|\s*\d+\s*-\s*\d+$")
_AFRET_DETECTOR = _afret_re.compile(r"^number of photons \((?P<detector>.+)\)$")


def _afret_role_of(name):
    low = str(name).strip().lower()
    for role, words in BURST_DETECTOR_ROLE_WORDS.items():
        # longest first: "delayed" must win over the donor list's bare "d"
        for word in sorted(words, key=len, reverse=True):
            if low == word or word in low.split():
                return role
    return ""


def gated_stream_columns(names):
    """The ALEX/PIE channels among a burst table's window x detector columns.

    ``S {window} {detector} (photons|kHz) | r0-r1`` columns are matched against
    the detectors named by ``Number of Photons ({detector})``. Photon counts win
    over rates (E and S are count ratios). The acceptor-excitation channel
    prefers a second acceptor detector when the setup lists one (the Seidel
    convention enters the red detector twice, once per window).

    Returns
    -------
    dict
        ``{"i_dd", "i_da", "i_aa"}`` column names for the roles resolved; empty
        without gated columns or when the donor-excitation pair is incomplete.
    """
    originals = [str(n) for n in names]
    detectors = [m.group("detector").strip() for m in
                 (_AFRET_DETECTOR.match(n.strip().lower()) for n in originals) if m]
    if not detectors:
        return {}
    gated, rates = {}, {}
    for name in originals:
        match = _AFRET_GATED.match(name.strip().lower())
        if not match:
            continue
        middle = match.group("middle")
        target = gated if match.group("unit") == "photons" else rates
        for detector in detectors:
            if middle == detector or middle.endswith(" " + detector):
                window = middle[: len(middle) - len(detector)].strip()
                if window:
                    target[(window, detector)] = name
                break
    for key, name in rates.items():
        gated.setdefault(key, name)
    if not gated:
        return {}
    pick = lambda role, items: next((c for c in items if _afret_role_of(c) == role), "")
    windows = list(dict.fromkeys(w for w, _ in gated))
    w_donor, w_acceptor = pick("donor", windows), pick("acceptor", windows)
    d_donor = pick("donor", detectors)
    acceptors = [d for d in detectors if _afret_role_of(d) == "acceptor"]
    d_da = acceptors[0] if acceptors else ""
    d_aa = next(iter(acceptors[1:]), d_da)
    out = {}
    for role, key in (("i_dd", (w_donor, d_donor)), ("i_da", (w_donor, d_da)),
                      ("i_aa", (w_acceptor, d_aa))):
        if all(key) and key in gated:
            out[role] = gated[key]
    return out if "i_dd" in out and "i_da" in out else {}


def guess_burst_columns(names, extra_hints=None):
    """Map the channel roles ``i_dd``/``i_da``/``i_aa``/``tau_f`` onto column names.

    Gated window x detector streams win when present
    (:func:`gated_stream_columns`); otherwise the name conventions in
    ``BURST_COLUMN_HINTS`` are matched, ``extra_hints`` (``{role: fragments}``)
    first. A column is never assigned to two roles; unmatched roles are left out.
    """
    names = list(names)
    out = gated_stream_columns(names)
    lowered = [(str(n), str(n).strip().lower()) for n in names]
    for role, hints in BURST_COLUMN_HINTS.items():
        if role in out:
            continue
        for hint in tuple((extra_hints or {}).get(role, ())) + tuple(hints):
            match = next((o for o, low in lowered if low == hint or hint in low), None)
            if match is not None and match not in out.values():
                out[role] = match
                break
    return out


def calibration_report(result, messages_before=()):
    """The printable summary of an :func:`auto_calibrate` result."""
    import math
    finite = lambda v: v is not None and math.isfinite(float(v))
    lines = ["Automatic FRET calibration", "=========================="]
    for key in ("alpha", "delta", "gamma", "beta"):
        u = result["uncertainties"].get(key)
        unc = f" ± {u:.4f}" if finite(u) else ""
        lines.append(f"  {key:<6s} = {result['factors'].get(key, float('nan')):.4f}{unc}")
    split = result.get("split")
    if split is not None:
        c = split["counts"]
        lines.append(f"  bursts: {c['donor_only']} donor-only, {c['acceptor_only']} acceptor-only, "
                     f"{c['fret']} FRET in {c['fret_populations']} population(s)")
        lines.append(f"  stoichiometry cuts ({split['method']}): "
                     f"{split['thresholds'][0]:.3f} / {split['thresholds'][1]:.3f}")
    labels = {"prior": "light path", "es": "E-S population fit", "lifetime": "static FRET line",
              "data": "data (adopted)", "posterior": "posterior"}
    for key, label in labels.items():
        value = result["gamma_estimates"].get(key)
        if finite(value):
            lines.append(f"  gamma [{label}] = {value:.4f}")
    species = result.get("species")
    if species and species["model_selection"]["selected"] == "species":
        values = ", ".join(f"{v:.4f}" for v in species["factors"]["gamma"]["values"])
        lines.append(f"  gamma per FRET population (species model, lower BIC) = {values}")
    for p in result.get("populations", []):
        tau = f", tau_f = {p['tau_f']:.3f} ns" if "tau_f" in p else ""
        dev = f", off-line by {p['deviation']:+.3f}" if "deviation" in p else ""
        lines.append(f"  population {p['label']}: n = {p['n']}, E = {p['E']:.3f} "
                     f"± {p['sigma_E']:.3f}, R = {p['distance']:.1f} Å{tau}{dev}")
    lines.extend(f"  ! {m}" for m in list(messages_before) + list(result.get("messages", [])))
    lines.append(f"  {'converged' if result.get('converged') else 'not converged'} "
                 f"after {result.get('iterations', 0)} iteration(s)")
    return "\n".join(lines)
