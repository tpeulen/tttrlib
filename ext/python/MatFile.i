// SPDX-License-Identifier: BSD-3-Clause
%{
#include "MatFile.h"
%}

// MATLAB Level-5 MAT-files as a flat tree of arrays. The scipy-shaped API --
// loadmat / savemat / whosmat with numpy arrays, dicts and scipy's squeeze
// rules -- is `tttrlib.matfile` (ext/python/matfile_module.py); MatFile is its
// plumbing and the byte-level door for other bindings.

%apply (unsigned char** ARGOUTVIEWM_ARRAY1, int* DIM1) {(unsigned char** out, int* n_out)}
%apply (unsigned char* IN_ARRAY1, int DIM1) {(const unsigned char* in_data, int n_data)}

%include "MatFile.h"
