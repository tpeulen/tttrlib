# `io/ps` — Photonscore LINCam Format I/O

This module implements reader support for Photonscore LINCam `.photons` (D7) files.

## Contents

- **`PhotonscoreD7.h` / `PhotonscoreD7.cpp`**: Photonscore D7 header and event reader.

## Provenance and licence

A modified C++ port of the Photonscore D7 reference implementation
([photonscore/d7](https://github.com/photonscore/d7), Apache-2.0, Copyright 2026 Photonscore GmbH)
and of [photonsfile](https://github.com/alex1075/photonsfile) (MIT, Copyright (c) 2026 Alex Hunt).
The upstream notices are in both source files and in the repository's `NOTICE`; the licence texts
are `licenses/Apache-2.0.txt` and `licenses/photonsfile-MIT.txt`. tttrlib's changes are BSD-3-Clause.

## Dependencies

- Depends on `io/base`, `util`.
