#!/usr/bin/env python
"""Record PAM's PDA histogram library for ``TestAgainstPam`` in ``test_ab_pda_reference.py``.

Reference: PAM (Schrimpf et al. 2018), https://gitlab.com/PAM-PIE/PAM at commit
7319d15d, file ``functions/PDAFit/histogram_library/PDA_histogram.cpp``. The MEX
source is compiled unmodified through a 15-line ``mex.h`` shim into a
stdin/stdout program and driven with P(F), p_ch1 and the two backgrounds; its
S1/S2 matrices (one species per call) are stored with the inputs in
``test/data/reference/pda_pam_histogram_reference.npz``. Mixtures are stored per
species: the test forms PAM's amplitude-weighted sum itself.

The checkout is not kept on disk. To regenerate, re-clone it with
``../chisurf/junk/clone.sh`` and run

    python test/python/pda/gen_ab_pda_pam_reference.py [--pam /path/to/PAM]

(``PAM_DIR`` in the environment works too; the default is ``../chisurf/junk/PAM``).
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from math import factorial

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(ROOT, "test", "data", "reference", "pda_pam_histogram_reference.npz")
DEFAULT_PAM = os.environ.get("PAM_DIR", os.path.join(ROOT, "..", "chisurf", "junk", "PAM"))
REL_SRC = os.path.join("functions", "PDAFit", "histogram_library", "PDA_histogram.cpp")

_MEX_SHIM = r"""
#pragma once
#include <cstdlib>
#include <cstdio>
#include <cstddef>
typedef size_t mwSize;
struct mxArray { double* data; size_t n; };
enum { mxDOUBLE_CLASS = 6 };
enum { mxREAL = 0 };
inline double mxGetScalar(const mxArray* a) { return a->data[0]; }
inline double* mxGetPr(const mxArray* a) { return a->data; }
inline void* mxCalloc(size_t n, size_t s) { return calloc(n, s); }
inline mxArray* mxCreateNumericMatrix(mwSize m, mwSize n, int, int) { mxArray* a = new mxArray; a->data = nullptr; a->n = m * n; return a; }
inline void mxSetData(mxArray* a, void* p) { a->data = static_cast<double*>(p); }
inline void mexErrMsgIdAndTxt(const char*, const char* m) { std::fprintf(stderr, "%s\n", m); std::exit(2); }
"""

_MAIN = r"""
#include "mex.h"
#include <vector>
#include <cstdio>
void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]);
int main() {
    unsigned Nmax; if (std::scanf("%u", &Nmax) != 1) return 1;
    std::vector<double> pN(Nmax + 1);
    for (unsigned i = 0; i <= Nmax; ++i) std::scanf("%lf", &pN[i]);
    double pG, Bg, Br, use; std::scanf("%lf %lf %lf %lf", &pG, &Bg, &Br, &use);
    double nm = Nmax;
    mxArray aN{&nm, 1}, apN{pN.data(), pN.size()}, apG{&pG, 1}, aBg{&Bg, 1}, aBr{&Br, 1}, aU{&use, 1};
    const mxArray* in[6] = {&aN, &apN, &apG, &aBg, &aBr, &aU};
    mxArray* out[1];
    mexFunction(1, out, 6, in);
    for (size_t i = 0; i < out[0]->n; ++i) std::printf("%.17g\n", out[0]->data[i]);
    return 0;
}
"""


def compile_pam(src, build):
    cxx = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if cxx is None:
        raise SystemExit("no C++ compiler")
    with open(os.path.join(build, "mex.h"), "w") as f:
        f.write(_MEX_SHIM)
    with open(os.path.join(build, "matrix.h"), "w") as f:
        f.write('#pragma once\n#include "mex.h"\n')
    with open(os.path.join(build, "main.cpp"), "w") as f:
        f.write(_MAIN)
    exe = os.path.join(build, "pam_pda")
    subprocess.run([cxx, "-std=c++17", "-O2", "-I", build, os.path.join(build, "main.cpp"), src, "-o", exe],
                   check=True)
    return exe


def poisson_pf(lam, nmax):
    p = np.array([np.exp(-lam) * lam ** i / factorial(i) for i in range(nmax + 1)])
    return p / p.sum()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pam", default=DEFAULT_PAM, help="PAM checkout (default: %(default)s)")
    args = ap.parse_args()
    src = os.path.join(os.path.abspath(args.pam), REL_SRC)
    if not os.path.exists(src):
        raise SystemExit(f"{src} not found -- re-clone PAM with ../chisurf/junk/clone.sh")
    with tempfile.TemporaryDirectory(prefix="pam_pda_gen_") as build:
        exe = compile_pam(src, build)

        def s1s2(nmax, pf, p1, bg1, bg2):
            p1, bg1, bg2 = float(p1), float(bg1), float(bg2)  # numpy 2 reprs as np.float64(...)
            inp = f"{nmax}\n" + " ".join(repr(float(x)) for x in pf) + f"\n{p1!r} {bg1!r} {bg2!r} 1\n"
            out = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.split()
            return np.array([float(x) for x in out]).reshape(nmax + 1, nmax + 1)

        rec = {}
        # single species
        nmax = 14
        pf = poisson_pf(5.0, nmax)
        p1s = np.array([0.05, 0.3, 0.5, 0.95])
        bgs = np.array([(0.0, 0.0), (1.5, 0.8), (3.0, 3.0), (0.0, 2.2)])
        rec.update(single_nmax=nmax, single_pf=pf, single_p_ch1=p1s, single_bg=bgs,
                   single_s1s2=np.array([[s1s2(nmax, pf, p1, *bg) for bg in bgs] for p1 in p1s]))
        # mixture, one PAM call per species
        nmax = 16
        pf = poisson_pf(6.0, nmax)
        amps, probs = np.array([0.5, 0.3, 0.2]), np.array([0.2, 0.55, 0.9])
        bgs = np.array([(0.0, 0.0), (2.0, 1.0)])
        rec.update(mix_nmax=nmax, mix_pf=pf, mix_amps=amps, mix_p_ch1=probs, mix_bg=bgs,
                   mix_species_s1s2=np.array([[s1s2(nmax, pf, p, *bg) for p in probs] for bg in bgs]))
        # bimodal P(F)
        nmax = 12
        pf = np.zeros(nmax + 1)
        pf[3] = 0.4
        pf[9] = 0.6
        rec.update(bimodal_nmax=nmax, bimodal_pf=pf, bimodal_p_ch1=0.35, bimodal_bg=np.array([0.7, 0.4]),
                   bimodal_s1s2=s1s2(nmax, pf, 0.35, 0.7, 0.4))
    rec["source"] = np.array("https://gitlab.com/PAM-PIE/PAM@7319d15d " + REL_SRC.replace(os.sep, "/"))
    np.savez_compressed(OUT, **rec)
    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    main()
