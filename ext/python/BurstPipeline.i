// SPDX-License-Identifier: BSD-3-Clause
//
// The fused burst pipeline. Every array it returns is a view of a buffer the result
// object owns -- ARGOUTVIEW, not ARGOUTVIEWM, so nothing is copied -- and the Python
// side (burstpipeline_support.py) ties each view's lifetime to that object, so a
// view cannot outlive what it points into.
%{
#include "BurstPipeline.h"
%}

%shared_ptr(tttrlib::BurstPipelineResult)
%shared_ptr(tttrlib::BurstPipeline)
%shared_ptr(tttrlib::StrictBackground)

%exception {
    try {
        $action
    } catch (const std::invalid_argument& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    } catch (const std::out_of_range& e) {
        SWIG_exception(SWIG_IndexError, e.what());
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    }
}

%apply (double** ARGOUTVIEW_ARRAY1, int* DIM1) {(double **view, int *n)};
%apply (long long** ARGOUTVIEW_ARRAY1, int* DIM1) {(long long **view, int *n)};
%apply (int** ARGOUTVIEW_ARRAY1, int* DIM1) {(int **view, int *n)};
%apply (long long* IN_ARRAY1, int DIM1) {(long long *first, int n_first), (long long *last, int n_last)};
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(double *backgrounds, int n_rows, int n_cols)};

// the raw view accessors; the public spelling (burstpipeline_support.py) keeps the owner
%rename(_column) tttrlib::BurstPipelineResult::column;
%rename(_index_column) tttrlib::BurstPipelineResult::index_column;
%rename(_decay_indptr) tttrlib::BurstPipelineResult::decay_indptr;
%rename(_decay_bins) tttrlib::BurstPipelineResult::decay_bins;
%rename(_decay_counts) tttrlib::BurstPipelineResult::decay_counts;
%rename(_decay_sum) tttrlib::BurstPipelineResult::decay_sum;
%rename(_outside_histogram) tttrlib::BurstPipelineResult::outside_histogram;
%rename(_histogram_view) tttrlib::StrictBackgroundResult::histogram_view;
%ignore tttrlib::BurstPipelineResult::nbytes;

#ifdef SWIGPYTHON
%pythoncode "./ext/python/burstpipeline_support.py"
%extend tttrlib::BurstPipelineResult { %pythoncode "./ext/python/BurstPipelineResult.py" }
%extend tttrlib::StrictBackgroundResult {
%pythoncode %{
    @property
    def histogram(self):
        """Kept-photon micro-time histograms, ``(len(histogram_channels()), length)`` int64 view."""
        flat = _bp_wrap(self, self._histogram_view())
        return flat.reshape(-1, max(1, int(self.length))) if flat.size else flat.reshape(0, max(1, int(self.length)))
%}
}
#endif

%include "BurstPipeline.h"

// %exception is global: end this scope by restoring the library-wide handler
// (MicrotimeLinearization.i installs it, every later interface relies on it),
// never by clearing it to nothing -- see the note at the bottom of Fdc2D.i and
// tools/check_exception_handlers.py. Keep this body identical to that one.
%exception {
    try {
        $action
    } catch (const std::invalid_argument& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    } catch (...) {
        SWIG_exception(SWIG_UnknownError, "Unknown exception");
    }
}

%clear (long long **view, int *n);
%clear (int **view, int *n);
%clear (long long *first, int n_first);
%clear (long long *last, int n_last);
%clear (double *backgrounds, int n_rows, int n_cols);
