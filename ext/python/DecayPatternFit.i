// SPDX-License-Identifier: BSD-3-Clause
%{
#include "DecayPatternFit.h"
%}

// Vector-of-vector patterns and the exception path both need std::exception
// mapped.
%exception tttrlib::decay_pattern_fit {
    try {
        $action
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    }
}

// Member vectors by value, not by pointer into the object -- the trap
// `DecayFit.i` documents. Without it,
// `decay_pattern_fit(...).amplitudes` reads a vector belonging to a
// `PatternFitResult` Python has already freed; it only appears to work when
// the result is bound to a name first.
%naturalvar tttrlib::PatternFitResult::amplitudes;

namespace tttrlib {};
%include "DecayPatternFit.h"
