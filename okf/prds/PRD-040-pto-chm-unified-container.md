# PRD-040 — PTO learns from the `.chm` container: one grammar for photons, images and structures

> **PRD #:** 040 · **Status:** proposal · **Created:** 2026-08-23 · **Owner:** tpeulen
> **Cross-repo:** the chimol side of this decision is recorded in
> `chimol/okf/architecture/chm-container.md` (envelope adoption) and
> `chimol/okf/references/pto-chm-convergence.md`.

## Naming (settled 2026-08-23)

**PTO — the container format (Thomas Peulen) — is the format for both.** The name
is a wink at PicoQuant's `.ptu` (PTO = PhoTon cOntainer; the containers it wraps
include PTU streams). There is one DocType, `"pto"`, and one grammar. Domains are
**profile suffixes** on the file name, which name the *primary* payload kind and
therefore which profile a consumer should expect:

| extension | profile | primary payloads |
|---|---|---|
| `.mmfbd.pto` | photons / fluorescence imaging | `tttr.stream`, image level-sets, detectors/counts |
| `.chm.pto` | molecular structures | chunked 16-byte GPU rows, chunk index, overlays |
| `.drot.pto` | dye rotamer libraries (imp.bff) | `drot.header`, `drot.template`, `drot.rows`, `drot.grid`, `drot.weights` |
| `.pto` | unspecified / mixed | anything — the grammar is the same |

A reader inspects `PtoKind` per object (as today); the suffix is for humans,
shells and file dialogs. Plain `.pto` stays valid everywhere and is what
cross-domain archives use.

## Adoption so far (2026-08-24)

A third profile landed the day after this was written, and it is evidence for
the central claim: **imp.bff's `.drot` rotamer libraries** moved from a private
brotli+tar to the envelope with **no new element ID** — the eight members are
`PtoKind` strings — and with a vendored core of its own (`include/Pto.h`,
~570 lines of C++ written from the spec; no dependency on tttrlib, per the
no-cross-repo-import rule). All 95 shipped libraries validate under
`test/tools/pto_ebml_check.cpp` with `--aligned`, and chimol's independent
Python walker reads them too — the shared conformance corpus working as
intended, three implementations deep. Their measurement is worth keeping here:
compressing each object separately rather than one stream over the whole
payload cost **nothing** (19.60 MB vs 19.64 MB over that corpus), because the
tar headers it replaces pay for the joint context given up.

## Summary

chimol adopted the PTO envelope (EBML, aligned payloads, uid-addressed reads) for its
`.chm` gigastructure container. This PRD is the reverse direction, and the actual goal:
**one container grammar to maintain**, where `.pto` (photons, images) and `.chm`
(structures) are profiles of the same element schemas rather than two formats. PTO
already owns the layer that is hard (envelope, alignment, rewrite/relocation, compact,
streamed writes). What it lacks is the layer `.chm` just designed: **typed indexes,
pyramid levels, sparse overlays**. Adopting those three element patterns makes the
grammar sufficient for both domains.

## What `.pto` takes from `.chm` (and why each earns its place)

1. **`pto.index` — a typed chunk index over a stream.**
   PRD-020 gave PTO targeted reads by *byte range* (`read(uid, at, n)`). The `.chm`
   pattern is one level up: an index element whose rows carry **min/max bounds plus
   payload references**, so a consumer asks a semantic question ("what is in this
   window?") and the index answers with offsets. For `tttr.stream` the bounds are
   temporal: rows of `(t_min, t_max, micro_time_range, det_mask, uid, offset, nbytes)`
   over fixed-size blocks of records. Time-range reads, windowed histogramming and
   burst search then touch only their blocks — the streaming-and-targeted-reads goal,
   completed at the semantic level byte ranges could not reach. For image payloads the
   same element is spatial (tile bounds), see (2).
   *Columns are per-`PtoKind`; the element schema (id → row layout → payload ref) is
   the shared thing.*

2. **Levels + per-level transforms for images (OME-Zarr semantics, in EBML).**
   Image objects become level sets: one logical object, elements per level
   (`pto.img.<uid>.L<n>` or level rows in the index), each chunked into tiles, plus
   per-level `coordinateTransformations` in metadata — the multiscale model chimol
   adopted from OME-Zarr and generalized for `.chm`'s `L0..Ln`. This replaces "an
   image is one big blob" with "an image is a pyramid a viewer can enter at any
   scale", which is what FLIM/CLSM mosaic browsing actually needs.

3. **`pto.ann.*` — sparse annotation overlays.**
   The `.chm` annotations pattern: an overlay *references* its source payload by uid
   and holds chunk-local indices + presentation metadata (`colors`, `properties`,
   `kind`) — never a copy of the data. For tttrlib: ROI polygons over images (tile-
   local index lists), flagged photon bursts / artifact windows over streams (block-
   local record ranges). Edits stay local; the index gains an `annotated` bit per row
   so "where are my annotations" is an index question.

4. **Sidecar discipline (no code, just order).**
   Heavy descriptive metadata rides a lazily-read sidecar element next to the payload
   it describes (`.chm`'s identity sidecars; PTO's equivalent: per-block routing/
   channel tables nobody reads unless decoding needs them).

## What stays domain-specific

- **Payload encodings**: TTTR bitfield records (lossless, consumer layout — already
  the `.chm` discipline), image tiles (chunked, codec per `PtoEncoding`), `.chm`'s
  16-byte GPU-decoded rows. The grammar never inspects payloads; `PtoEncoding`
  already names them.
- **Index column sets**: temporal for streams, spatial for structures/tiles. The
  grammar specifies the *element*, the profiles specify the columns.

## The one format

- **One spec**: the PTO binary-decoding spec generalises to the element grammar
  (envelope + attachments + index + levels + overlays); this document's schemas
  fold into it as DocTypeVersion 2. One DocType string, `"pto"`, for everything —
  the domain lives in the profile suffix (`.mmfbd.pto`, `.chm.pto`) and in each
  object's `PtoKind`, not in the container.
- **One implementation core per language, no cross-repo import** — *amended
  2026-09-07 (T-20260907-07):* **the C++ core is ptolib**
  (https://github.com/tpeulen/ptolib), one header that tttrlib
  (`thirdparty/ptolib/`) and imp.bff (`include/internal/`) vendor verbatim with a
  provenance note and a byte-compare test; neither repo imports the other, and the
  two hand-written C++ walkers that had already drifted (tttrlib could not open a
  `.drot.pto`) are gone. Python walkers (chimol, chisurf) still follow the rule as
  written: each vendors its own small walker against the spec. Divergence is held
  down by a shared conformance corpus: the `.pto` EBML checker (now
  `ptolib/tools/pto_ebml_check.cpp`) gains the new element IDs, and ptolib's
  `tests/data/` holds the golden files both writers must keep opening.
- **PTO's paid-for lessons transfer unchanged**: 8-byte alignment, fixed-octet sizes
  for anything rewritten in place (the `FileUID` corruption), streamed writes,
  `compact(tight/reserve)`.

## Acceptance

1. Spec: element schemas for index / levels / overlays written into the pto spec;
   `pto_ebml_check` validates them.
2. tttrlib: one `tttr.stream` written with a `pto.index`; a time-range read touches
   only the covered blocks (asserted by bytes-read accounting).
3. chimol: `.chm` builder emits the same index/overlay element shapes (its chunk
   table already matches modulo column names).
4. Golden container exercising every element kind, loadable by both checkers.
