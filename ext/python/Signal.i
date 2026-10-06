// SPDX-License-Identifier: BSD-3-Clause
%{
#include "Signal.h"
%}

// lfilter, and FITPACK's fixed-knot curfit + splev (Signal.h), as flat
// array-in/array-out entry points. The scipy-shaped API is tttrlib.signal and
// tttrlib.interpolate (ext/python/signal_module.py, interpolate_module.py);
// the `signal_*` names are their plumbing.
%apply (double* IN_ARRAY1, int DIM1) {
    (const double* b, int n_b),
    (const double* a, int n_a),
    (const double* src, int n_src),
    (const double* x, int n_x),
    (const double* y, int n_y),
    (const double* w, int n_w),
    (const double* interior, int n_interior),
    (const double* t, int n_t),
    (const double* c, int n_c)
}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {
    (double** out, int* n_out),
    (double** out_t, int* n_out_t),
    (double** out_c, int* n_out_c)
}
%apply (double* OUTPUT) {(double* fp)}

%exception signal_lfilter {
    try {
        $action
    } catch (const std::invalid_argument& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    } catch (const std::bad_alloc&) {
        SWIG_exception(SWIG_MemoryError, "out of memory");
    }
}

%include "Signal.h"

%exception;
