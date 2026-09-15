// SPDX-License-Identifier: BSD-3-Clause
%{
#include "Registry.h"
%}

// The mechanism (descriptor and table) first; the table template is C++-side only.
%ignore tttrlib::AlgorithmTable;
%include "RegistryCore.h"
%include "Registry.h"

#ifdef SWIGPYTHON
%pythoncode "./ext/python/pipeline_support.py"

// registry(), describe(), resolve(), defaults(), compose(), api_index(): one module-agnostic
// file, shared with every library that uses RegistryCore.h (imp.bff embeds the same file).
%pythoncode "./ext/python/registry_access.py"
#endif
