// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_SIGNAL_H
#define TTTRLIB_SIGNAL_H
// Validation: A/B-TESTED 2026-10-06 -- vs scipy 1.18 (test/python/misc/
//   test_signal_parity.py): lfilter bit-identical over random IIR/FIR
//   filters; splrep(t=...)/splev bit-identical over random data and knots.
//
// Signal.h -- the curve half of what ChiSurf took from scipy.signal and
// scipy.interpolate: a direct-form-II-transposed IIR/FIR filter (lfilter) and
// the fixed-knot least-squares B-spline fit (splrep with given knots) plus
// its evaluation (splev).
//
// Ports, not re-derivations: lfilter is the inner loop of scipy's
// _lfilter.cc (`filt`), and the spline pair is FITPACK as scipy carries it in
// C (dfitpack.c: curfit with iopt = -1 -> fpcurf's least-squares pass,
// fpchec, fpbspl, fpgivs, fprota, fpback; splev). BSD-3-Clause, Copyright the
// SciPy developers; FITPACK by P. Dierckx. The order of every multiply-add
// is kept, and Signal.cpp compiles with FP contraction ON as scipy's wheels
// do, so the numbers are scipy's to the bit.
//
// The scipy-shaped API (argument checks, dtype, the tck tuple) is the Python
// layer: `tttrlib.signal` and `tttrlib.interpolate`.

#include <vector>

/*!
 * \brief Filter one line with the rational transfer function b(z)/a(z).
 *
 * Direct form II transposed, initial rest: scipy.signal.lfilter(b, a, x)
 * for 1-D float64 input. b and a are zero-filled to max(len(a), len(b)) and
 * divided by a[0] once, as scipy does.
 *
 * \throws std::invalid_argument if b or a is empty or a[0] == 0.
 */
void signal_lfilter(const double* b, int n_b, const double* a, int n_a,
                    const double* src, int n_src, double** out, int* n_out);

/*!
 * \brief FITPACK curfit with fixed interior knots (scipy's splrep(t=...)).
 *
 * Least-squares spline of degree k through (x, y) with weights w on
 * [xb, xe], interior knots `interior` (strictly inside, increasing). Writes
 * the full knot vector (k+1 copies of xb, the interior knots, k+1 copies of
 * xe) to `out_t` and the coefficients, length len(out_t) as scipy returns
 * them (trailing k+1 zero), to `out_c`.
 *
 * \return FITPACK's ier: 0 is success; 10 a bad argument (fpchec's
 *         Schoenberg-Whitney conditions included).
 */
int signal_curfit_fixed(const double* x, int n_x, const double* y, int n_y,
                        const double* w, int n_w, double xb, double xe, int k,
                        const double* interior, int n_interior,
                        double** out_t, int* n_out_t, double** out_c, int* n_out_c,
                        double* fp);

/*!
 * \brief FITPACK splev: evaluate a spline (t, c, k) at x.
 *
 * \param ext 0 extrapolate, 1 return 0, 2 raise (returns ier 1), 3 clamp
 *        to the boundary value -- scipy's `ext`.
 * \return FITPACK's ier.
 */
int signal_splev(const double* t, int n_t, const double* c, int n_c, int k,
                 const double* x, int n_x, int ext, double** out, int* n_out);

#endif  // TTTRLIB_SIGNAL_H
