---
type: Validation
title: Simultaneous scanner frame boundaries
description: Explicit line stops preserve complete frames and photon identities.
status: done
---

# Where to pick this up

Verified 2026-10-08. Re-derive with the five targeted CLSM files listed below
using ChiSurf's canonical test environment. Hardware-data cases skip when their
external fixtures are absent; the synthetic identity cases always run.
The original Flow PTU remains byte-identical; never substitute padded frames
or fabricated TIFF input as reader acceptance.

A stop marker at the next frame/start timestamp closes the preceding line.
Backtracking from a new frame must stop at that distinct configured line stop.
Previously it crossed the stop, paired stop with start and created zero-duration
lines, dropping the first complete frame.

The original C++ overload remains available. The stop-aware overload is hidden
from SWIG, preserving Python positional and keyword calls. Sentinel 255 and
start-only scanning retain their existing policy.

# Verification

Four old-provider identity failures become passing. The repaired canonical
provider passed 29 tests, 5 hardware-fixture skips in 0.70 seconds across
`test_frame_marker_boundaries.py`, `test_CLSM_bh_markers.py`,
`test_CLSM_single_frame_ptu.py`, `test_clsm_cross_language.py` and
`test_CLSM_lazy_fill.py`. Synthetic checks prove shape, every pixel's counts,
mean microtime and exact photon indices without duplication.

Flow PTU SHA256 `f0f25d3ffc43671fa1f53c8e581ff9def8ae8020a08f6cd4e6395e2c2551e05e`
now yields 50x64x64 frames and all 715,744 photons. Its actual native workflow
produces 25/25 retained arrows, zero escaped tiles. The conservative estimator's
profile peak is 0.7684477 of declared synthetic truth: this proves reader
integrity and usable analysis, not exact velocity truth.

Installed version: 0.27.0. CLSM compiled library SHA256:
`ae0d07c651cdfc3bbf0a68cea94ea59c0595b26ea589f83c4c6c2c500bdcda51`.
Wheel SHA256:
`dc944d9bb105d84cc0775f9877df6667dda09e95cc897d06ed9371247b61a20c`.
Source builds use ChiSurf's declared test prefix without shadow imports.
See [algorithm register](algorithm-validation.md) and the module README.
