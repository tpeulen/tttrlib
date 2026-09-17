#!/usr/bin/env python
"""Record PAM's CUSUM/SPRT burst search for ``TestCusumSprtAgainstZhangYang``.

Reference: PAM (Schrimpf et al. 2018), https://gitlab.com/PAM-PIE/PAM at commit
7319d15d, function ``CUSUM_burstsearch`` in ``PAM.m`` (up to
``BurstSearch_Preview``). The function body is extracted verbatim, with only
the ``global FileInfo`` line dropped and ``FileInfo.ClockPeriod`` passed as an
argument, and run in Octave on three bursty streams (``bursty_stream`` seeds
0-2 of ``test_ab_burst_reference.py``) at IB = 3 kHz, IT = 15 kHz and
1e-7 s per tick. Stored per seed: the photon ticks and PAM's 0-based
(START, STOP) photon indices, in
``test/data/reference/cusum_pam_reference.npz``.

The checkout is not kept on disk. To regenerate, re-clone it with
``../chisurf/junk/clone.sh`` and run

    python test/python/burstfilter/gen_ab_cusum_pam_reference.py [--pam /path/to/PAM]

(``PAM_DIR`` in the environment works too; the default is ``../chisurf/junk/PAM``).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(ROOT, "test", "data", "reference", "cusum_pam_reference.npz")
DEFAULT_PAM = os.environ.get("PAM_DIR", os.path.join(ROOT, "..", "chisurf", "junk", "PAM"))

sys.path.insert(0, HERE)
from test_ab_burst_reference import RES, bursty_stream  # noqa: E402

SEEDS = (0, 1, 2)
IB_KHZ, IT_KHZ = 3.0, 15.0


def extract_cusum(pam_m):
    src = open(pam_m, encoding="utf-8", errors="replace").read().splitlines()
    i0 = next(i for i, l in enumerate(src) if l.startswith("function [START,STOP] = CUSUM_burstsearch"))
    i1 = next(i for i in range(i0 + 1, len(src)) if src[i].startswith("function BurstSearch_Preview"))
    body = [l for l in src[i0 + 1:i1] if not l.startswith("global FileInfo")]
    body = [l.replace("FileInfo.ClockPeriod", "ClockPeriod") for l in body]
    return "function [START,STOP] = pam_cusum_ref(Photons,IB,IT,ClockPeriod)\n" + "\n".join(body) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pam", default=DEFAULT_PAM, help="PAM checkout (default: %(default)s)")
    args = ap.parse_args()
    pam_m = os.path.join(os.path.abspath(args.pam), "PAM.m")
    octave = shutil.which("octave")
    if not os.path.exists(pam_m):
        raise SystemExit(f"{pam_m} not found -- re-clone PAM with ../chisurf/junk/clone.sh")
    if octave is None:
        raise SystemExit("octave not on PATH")
    rec = {}
    with tempfile.TemporaryDirectory(prefix="pam_cusum_gen_") as d:
        with open(os.path.join(d, "pam_cusum_ref.m"), "w") as f:
            f.write(extract_cusum(pam_m))
        for seed in SEEDS:
            ticks, _ = bursty_stream(seed, two_channels=False)
            np.savetxt(os.path.join(d, "photons.txt"), ticks, fmt="%d")
            cmd = (f"Photons=load('photons.txt'); [S,E]=pam_cusum_ref(Photons,{IB_KHZ},{IT_KHZ},{RES}); "
                   f"dlmwrite('out.txt',[S E],' ');")
            subprocess.run([octave, "--no-gui", "-q", "--eval", cmd], cwd=d, check=True,
                           capture_output=True, timeout=600)
            pam = np.loadtxt(os.path.join(d, "out.txt"), dtype=np.int64, ndmin=2) - 1  # 1-based
            rec[f"seed{seed}_ticks"] = np.asarray(ticks, dtype=np.int64)
            rec[f"seed{seed}_pam_start_stop"] = pam
    rec.update(seeds=np.array(SEEDS), IB_khz=IB_KHZ, IT_khz=IT_KHZ, resolution=RES,
               source=np.array("https://gitlab.com/PAM-PIE/PAM@7319d15d PAM.m CUSUM_burstsearch"))
    np.savez_compressed(OUT, **rec)
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
