// SPDX-License-Identifier: BSD-3-Clause
%{#include "AccurateFret.h"%}

// the raw kernels take flat vectors and return structs; the Python layer in
// AccurateFret.py gives them numpy-in / dict-out signatures under the public names
%rename(_afret_apparent_es) tttrlib::apparent_es;
%rename(_afret_corrected_es) tttrlib::corrected_es;
%rename(_afret_corrected_es_matrix) tttrlib::corrected_es_matrix;
%rename(_afret_corrected_es_general) tttrlib::corrected_es_general;

namespace tttrlib {};
%include "AccurateFret.h"

#ifdef SWIGPYTHON
%pythoncode "./ext/python/AccurateFret.py"
#endif
