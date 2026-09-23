# SPDX-License-Identifier: BSD-3-Clause
"""Import chisurf's own accurate-FRET modules, when chisurf still has them.

Used only to check the transcriptions in this package against the original.
chisurf pulls in IMP.bff at import time; a half-rebuilt IMP.bff (Python
proxy newer than its extension) must not hide chisurf's pure-numpy code, so a
broken IMP.bff is replaced by an empty stub for the import.
"""
import importlib
import sys
import types


def load(name, *required):
    """Return ``chisurf.<name>``, or ``None`` when chisurf, that module or any of
    the ``required`` attributes is gone (the algorithms moved to tttrlib)."""
    stubbed = False
    try:
        import IMP.bff  # noqa: F401
    except Exception:
        for key in [k for k in sys.modules if k == "IMP.bff" or k.startswith("IMP.bff.")]:
            del sys.modules[key]
        sys.modules["IMP.bff"] = types.ModuleType("IMP.bff")
        stubbed = True
    try:
        module = importlib.import_module("chisurf." + name)
        return module if all(hasattr(module, a) for a in required) else None
    except Exception:
        return None
    finally:
        if stubbed:
            # later importers must see the real failure, not the stub
            sys.modules.pop("IMP.bff", None)
