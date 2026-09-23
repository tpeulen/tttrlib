// SPDX-License-Identifier: BSD-3-Clause
%{
#include "DecayConvolution.h"
%}

// The %inline validation helpers below use Python's PyErr_Format. Provide a
// portable stderr shim for non-Python targets (R, Java) so the SAME %inline
// bodies compile unchanged; the Python build is unaffected (this block is
// excluded at SWIG-generation time when SWIGPYTHON is defined).
#ifndef SWIGPYTHON
%{
#include <cstdarg>
#include <cstdio>
#ifndef PyExc_ValueError
#define PyExc_ValueError 0
#endif
static inline void PyErr_Format(int, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    vfprintf(stderr, fmt, args); fputc('\n', stderr);
    va_end(args);
}
%}
#endif


// manually added instead of including header file as all other functions
%apply (double* INPLACE_ARRAY1, int DIM1) {
    (double* fit, int n_fit),
    (double *model, int n_model),
    (double *time_axis, int n_time_axis),
    (double *lifetime_spectrum, int n_lifetime_spectrum),
    (double *data, int n_data),
    (double* w_sq, int n_w_sq),
    (double* x, int n_x),
    (double* decay, int n_decay),
    (double* irf, int n_irf),
    (double* lamp, int n_lamp),
    (double* lampsh, int n_lampsh),
    (double *instrument_response_function, int n_instrument_response_function)
}

// The Jacobian is the one 2D buffer here. In-place like `fit` above, and for
// the same reason: the caller in a fit loop allocates once and reuses.
%apply (double* INPLACE_ARRAY2, int DIM1, int DIM2) {
    (double* jacobian, int n_jac1, int n_jac2)
}

void add_pile_up_to_model(
        double* model, int n_model,
        double* decay, int n_decay,
        double repetition_rate,
        double instrument_dead_time,
        double measurement_time,
        std::string pile_up_model = "coates",
        int start = 0,
        int stop = -1
);


//// rescale
//////////////
%ignore rescale;
%rename (rescale) my_rescale;
%inline %{
double my_rescale(
        double* fit, int n_fit,
        double* decay, int n_decay,
        int start = 0,
        int stop = -1
){
    if (n_fit != n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_decay);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_decay;
    }
    if (start >= n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_decay);
    }
    if (stop > n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_decay);
    }
    double scale = 0.0;
    rescale(fit, decay, &scale, start, stop);
    return scale;
}
%}

//// rescale_w
////////////////
%ignore rescale_w;
%rename (rescale_w) my_rescale_w;
%inline %{
double my_rescale_w(
        double* fit, int n_fit,
        double* decay, int n_decay,
        double* w_sq, int n_w_sq,
        int start = 0,
        int stop = -1
){
    if (n_fit != n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_decay);
    }
    if (n_decay != n_w_sq) {
        PyErr_Format(PyExc_ValueError,
                     "Weight and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_decay, n_w_sq);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_decay;
    }
    if (start > n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_decay);
    }
    if (stop > n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_decay);
    }
    double scale = 0.0;
    rescale_w(fit, decay, w_sq, &scale, start, stop);
    return scale;
}
%}

/// rescale_w_bg
///////////////////
%ignore rescale_w_bg;
%rename (rescale_w_bg) my_rescale_w_bg;
%inline %{
double my_rescale_w_bg(
        double* fit, int n_fit,
        double* decay, int n_decay,
        double* w_sq, int n_w_sq,
        double bg,
        int start = 0,
        int stop = -1
){
    if (n_fit != n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_decay);
    }
    if (n_decay != n_w_sq) {
        PyErr_Format(PyExc_ValueError,
                     "Weight and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_decay, n_w_sq);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_decay;
    }
    if (start > n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_decay);
    }
    if (stop > n_decay) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_decay);
    }
    double scale = 0.0;
    rescale_w_bg(fit, decay, w_sq, bg, &scale, start, stop);
    return scale;
}
%}

//// fconv
///////////////////
%ignore fconv;
%rename (fconv) my_fconv;
%inline %{
void my_fconv(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* x, int n_x,
        int start = 0,
        int stop = -1,
        double dt = 1.0
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_irf;
    }
    if (start > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_irf);
    }
    if (stop > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_irf);
    }
    fconv(fit, x, irf, n_x / 2, start, stop, dt);
}
%}


//// fconv_simd
///////////////////
// Deprecated alias: fconv already picks the best (SIMD or scalar) kernel at
// runtime, so a separate _simd name promises a choice the caller does not
// have (BUGS 2026-08-11). Shim kept for one release.
%ignore fconv_simd;
%rename (fconv_simd) my_fconv_simd;
%feature("pythonprepend") my_fconv_simd %{
    import warnings
    warnings.warn(
        "fconv_simd is deprecated: fconv already selects the best "
        "(SIMD or scalar) kernel at runtime; call fconv.",
        DeprecationWarning, stacklevel=2)
%}
%inline %{
void my_fconv_simd(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* x, int n_x,
        int start = 0,
        int stop = -1,
        double dt = 1.0
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_irf;
    }
    if (start > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_irf);
    }
    if (stop > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_irf);
    }
    fconv(fit, x, irf, n_x / 2, start, stop, dt);
}
%}


//// fconv_per
///////////////////
%ignore fconv_per;
%rename (fconv_per) my_fconv_per;
%inline %{
void my_fconv_per(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* x, int n_x,
        double period,
        int start = 0,
        int stop = -1,
        double dt = 1.0
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_fit;
    }
    if (start > n_fit) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_fit);
    }
    if (stop > n_fit) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_fit);
    }
    fconv_per(fit, x, irf, n_x / 2, start, stop, n_irf, period, dt);
}
%}

//// fconv_per_cs_jacobian
///////////////////
// Exposed by hand, like the rest of this file, so the argument order is the
// one a Python caller wants (buffers, then data, then settings) rather than
// the C order.
%feature("docstring") my_fconv_per_cs_jacobian
"Model curve and its exact derivatives, in one pass.

Fills ``fit`` with the periodic decay of ``x`` against ``irf`` shifted by
``time_shift``, and ``jacobian`` with its derivatives: one row per channel, one
column per parameter. Columns ``0 .. len(x)-1`` are the entries of ``x`` in its
own interleaved order (``a0, tau0, a1, tau1, ...``) and the last column is
``d fit / d time_shift``, so ``jacobian`` must have ``len(x) + 1`` columns.

The derivatives are forward-mode automatic differentiation of the same
recursion that produces the curve -- exact to round-off, not a finite
difference -- and cost ``ceil((len(x)+1)/8)`` passes rather than the
``2*(len(x)+1)`` model evaluations a central-difference gradient needs.

Both output arrays are written in place, so a fit loop allocates them once."

%rename (fconv_per_cs_jacobian) my_fconv_per_cs_jacobian;
%inline %{
void my_fconv_per_cs_jacobian(
        double* fit, int n_fit,
        double* jacobian, int n_jac1, int n_jac2,
        double* irf, int n_irf,
        double* x, int n_x,
        double period,
        double time_shift = 0.0,
        int conv_stop = -1,
        int stop = -1,
        double dt = 1.0
){
    if (conv_stop < 0) conv_stop = n_fit - 1;
    fconv_per_cs_jacobian(fit, n_fit, jacobian, n_jac1, n_jac2, x, n_x,
                          irf, n_irf, period, time_shift, conv_stop, stop, dt);
}
%}

//// fconv_per_simd
///////////////////
// Deprecated alias of fconv_per; see fconv_simd above.
%ignore fconv_per_simd;
%rename (fconv_per_simd) my_fconv_per_simd;
%feature("pythonprepend") my_fconv_per_simd %{
    import warnings
    warnings.warn(
        "fconv_per_simd is deprecated: fconv_per already selects the best "
        "(SIMD or scalar) kernel at runtime; call fconv_per.",
        DeprecationWarning, stacklevel=2)
%}
%inline %{
void my_fconv_per_simd(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* x, int n_x,
        double period,
        int start = 0,
        int stop = -1,
        double dt = 1.0
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_irf;
    }
    if (start > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_irf);
    }
    if (stop > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_irf);
    }
    fconv_per(fit, x, irf, n_x / 2, start, stop, n_irf, period, dt);
}
%}


//// fconv_per_cs
///////////////////
%ignore fconv_per_cs;
%rename (fconv_per_cs) my_fconv_per_cs;
%inline %{
void my_fconv_per_cs(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* x, int n_x,
        double period,
        int conv_stop = -1,
        int stop = -1,
        double dt = 1.0
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if(stop < 0){
        stop = n_irf - 1;
    }
    if(conv_stop < 0){
        // The convolution loop runs i <= conv_stop and touches lamp[i] and
        // fit[i], so the last valid value is n_irf - 1. Defaulting to n_irf
        // read and wrote one element past both buffers -- survivable in the
        // scalar path, but a hard crash once a SIMD kernel is used.
        conv_stop = n_irf - 1;
    }
    if (conv_stop >= n_irf) {
        conv_stop = n_irf - 1;
    }
    if (stop >= n_irf) {
        stop = n_irf - 1;
    }
    fconv_per_cs(fit, x, irf, n_x / 2, stop, n_irf, period, conv_stop, dt);
}
%}

//// fconv_ref
///////////////////
%ignore fconv_ref;
%rename (fconv_ref) my_fconv_ref;
%inline %{
void my_fconv_ref(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* x, int n_x,
        double tauref,
        int start = 0,
        int stop = -1,
        double dt = 1.0
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_irf;
    }
    if (start > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_irf);
    }
    if (stop > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_irf);
    }
    // dt used to be dropped here (C++ default 0.05 regardless of the
    // argument); found by the A/B vs the trapezoid sum, 2026-08-17.
    fconv_ref(fit, x, irf, n_x / 2, start, stop, tauref, dt);
}
%}

//// sconv
///////////////////
%ignore sconv;
%rename (sconv) my_sconv;
%inline %{
void my_sconv(
        double* fit, int n_fit,
        double* irf, int n_irf,
        double* model, int n_model,
        int start = 0,
        int stop = -1
){
    if (n_fit != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and decay array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_fit, n_irf);
    }
    if (n_model != n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Model and fit array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_model, n_irf);
    }
    if(start < 0){
        PyErr_Format(PyExc_ValueError,
                     "Start index needs to be larger or equal to zero."
        );
    }
    if(stop < 0){
        stop = n_irf;
    }
    if (start > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Start index (%d) too large for array of lengths (%d).",
                     start, n_irf);
    }
    if (stop > n_irf) {
        PyErr_Format(PyExc_ValueError,
                     "Stop index (%d) too large for array of lengths (%d).",
                     stop, n_irf);
    }
    sconv(fit, model, irf, start, stop);
}
%}

//// shift_lamp
///////////////////
%ignore shift_lamp;
%rename (shift_lamp) my_shift_lamp;
%inline %{
void my_shift_lamp(
        double* lamp, int n_lamp,
        double* lampsh, int n_lampsh,
        double ts = 0.0,
        double out_value=0.0
){
    if (n_lamp != n_lampsh) {
        PyErr_Format(PyExc_ValueError,
                     "IRF and shifted IRF array should have same length. "
                     "Arrays of lengths (%d,%d) given",
                     n_lamp, n_lampsh);
    }
    shift_lamp(lampsh, lamp, ts, n_lamp, out_value);
}
%}



%include "DecayConvolution.h"
