// SPDX-License-Identifier: BSD-3-Clause
%{
#include "BurstVariance.h"
%}

// Burst boundaries arrive as one NumPy int array (Python) / numeric matrix (R) / long[] (Java);
// the typemap also serves the HMMs (included after this file). The time axis is one int64 buffer.
%apply (long long* IN_ARRAY2, int DIM1, int DIM2) {(long long* bursts, int n_bursts, int n_cols)};
%apply (long long* IN_ARRAY1, int DIM1) {(long long* time_axis, int n_time_axis)};

%apply (unsigned char* IN_ARRAY1, int DIM1) {(unsigned char* mask_a, int n_mask_a), (unsigned char* mask_b, int n_mask_b)};

TTTRLIB_NOGIL(tttrlib::BurstVariance::compute)
TTTRLIB_NOGIL(tttrlib::BurstVariance::static_bound)

#ifdef SWIGPYTHON
%feature("pythonappend") tttrlib::BurstVariance::static_bound %{
    import numpy as _np
    val = _np.asarray(val, dtype=_np.float64)
%}
%feature("pythonappend") tttrlib::BurstVariance::shot_noise_sd %{
    import numpy as _np
    val = _np.asarray(val, dtype=_np.float64)
%}
%feature("pythonappend") tttrlib::BurstVariance::classify %{
    import numpy as _np
    val = _np.asarray(val, dtype=_np.int64)
%}
#endif

%include "BurstVariance.h"

%extend tttrlib::BurstVariance {
// A non-overloaded entry point for the burst-array form (see BVA.i: R's overload dispatcher
// cannot select an overload taking a matrix).
    void compute_bursts(long long* bursts, int n_bursts, int n_cols,
                        int slice_mode, int photons_per_slice, double window_ticks,
                        double align_ticks, double align_offset,
                        int min_photons, int min_slices, int ddof) {
        $self->compute(bursts, n_bursts, n_cols, slice_mode, photons_per_slice, window_ticks,
                       align_ticks, align_offset, min_photons, min_slices, ddof);
    }
}

#ifdef SWIGPYTHON
%extend tttrlib::BurstVariance {
    %pythoncode %{
    @property
    def mean(self):
        """Per-burst mean of the slice ratios (NaN: untested)."""
        import numpy as np
        return np.asarray(self.get_mean(), dtype=float)

    @property
    def sd(self):
        """Per-burst SD of the slice ratios (NaN: untested)."""
        import numpy as np
        return np.asarray(self.get_sd(), dtype=float)

    @property
    def ratio(self):
        """Per-burst pooled ratio sum n_A / sum N (NaN: untested)."""
        import numpy as np
        return np.asarray(self.get_ratio(), dtype=float)

    @property
    def chi2(self):
        """Per-burst binomial chi-square (NaN: untested or pooled ratio 0 or 1)."""
        import numpy as np
        return np.asarray(self.get_chi2(), dtype=float)

    @property
    def n_slices(self):
        """Counted slices per burst."""
        import numpy as np
        return np.asarray(self.get_n_slices(), dtype=np.int64)

    @property
    def mean_slice_photons(self):
        """Mean photon number of the counted slices per burst (NaN: untested)."""
        import numpy as np
        return np.asarray(self.get_mean_slice_photons(), dtype=float)

    @property
    def slice_photons(self):
        """Photon number of every counted slice of the tested bursts, in burst order."""
        import numpy as np
        return np.asarray(self.get_slice_photons(), dtype=np.int64)

    @property
    def slice_a(self):
        """Stream-A photon number of every counted slice."""
        import numpy as np
        return np.asarray(self.get_slice_a(), dtype=np.int64)

    @property
    def slice_owner(self):
        """Burst index of every counted slice."""
        import numpy as np
        return np.asarray(self.get_slice_owner(), dtype=np.int64)

    @property
    def pooled(self):
        """Per-burst statistic of the pooled counts (NaN: untested)."""
        import numpy as np
        return np.asarray(self.get_pooled_statistic(), dtype=float)

    @property
    def slice_duration(self):
        """Duration of every counted slice in time-axis units."""
        import numpy as np
        return np.asarray(self.get_slice_duration(), dtype=float)

    @property
    def tested(self):
        """True where a burst has ``min_slices`` counted slices."""
        import numpy as np
        return np.asarray(self.get_tested(), dtype=bool)
    %}
}

#endif
