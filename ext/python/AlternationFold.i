// SPDX-License-Identifier: BSD-3-Clause
%{
#include "AlternationFold.h"
%}

// fold_with_phases / fold_macro_times hand back a malloc'ed array numpy owns;
// (unsigned long long** output, int* n_output) is applied in misc_types.i.
%apply (unsigned long long* IN_ARRAY1, int DIM1) {(unsigned long long* macro_times, int n_macrotimes)}
%apply (unsigned short* IN_ARRAY1, int DIM1) {(unsigned short* micro_times, int n_microtimes)}

// fold_unchecked skips the phase range check; Python gets fold() only.
%ignore AlternationFold::fold_unchecked;

%include "AlternationFold.h"

%extend AlternationFold {
%pythoncode %{
    period = property(get_period)
    slot_starts = property(get_slot_starts)
    slot_length = property(get_slot_length)
    tick_scale = property(get_tick_scale)
    common_window_length = property(get_common_window_length)

    def __repr__(self):
        return (f"AlternationFold(period={self.get_period()}, "
                f"slot_starts={list(self.get_slot_starts())}, tick_scale={self.get_tick_scale():g})")
%}
}
