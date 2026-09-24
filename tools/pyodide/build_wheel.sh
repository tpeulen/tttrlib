#!/usr/bin/env bash
# Build tttrlib as a Pyodide wheel (wasm32-emscripten).
#
#   tools/pyodide/build_wheel.sh [outdir]      # default outdir: dist/pyodide
#
# Produces dist/pyodide/tttrlib-<version>-cp313-cp313-pyodide_2025_0_wasm32.whl.
#
# The toolchain lives outside the checkout and is installed on first use:
#   PYODIDE_VENV   a Python 3.13 venv holding pyodide-build  (~/opt/pyodide-venv)
#   EMSDK_DIR      emsdk, with the emscripten Pyodide needs  (~/opt/emsdk)
#   PYODIDE_XBUILDENV  the Pyodide cross-build environment    (~/opt/pyodide-xbuildenv)
#
# The Pyodide version is pinned to what the web hosts load (chimol's index.html
# pulls v0.28.0 from the CDN); the emscripten version is not a free choice, it
# is whatever that Pyodide release was built with, and is read back from the
# cross-build environment rather than repeated here.
#
# What differs from a native wheel (no HDF5, no OpenMP, static modules) is in
# pyproject.toml under [[tool.scikit-build.overrides]] -- pyodide-build sets
# PYODIDE=1, which is what selects it.
set -euo pipefail

PYODIDE_VERSION="${PYODIDE_VERSION:-0.28.0}"
# pyodide-build names the platform tag, so it is pinned. 0.39 writes the PEP 783
# `pyemscripten_2025_0` tag, which micropip 0.10 (the one Pyodide 0.28 ships)
# does not recognise; 0.30.9 writes `pyodide_2025_0`, the tag 0.28 installs.
PYODIDE_BUILD_VERSION="${PYODIDE_BUILD_VERSION:-0.30.9}"
PYODIDE_VENV="${PYODIDE_VENV:-$HOME/opt/pyodide-venv}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/opt/emsdk}"
export PYODIDE_XBUILDENV_PATH="${PYODIDE_XBUILDENV:-$HOME/opt/pyodide-xbuildenv}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTDIR="${1:-$ROOT/dist/pyodide}"

if [[ ! -x "$PYODIDE_VENV/bin/python" ]]; then
    echo "==> creating $PYODIDE_VENV"
    if command -v uv >/dev/null; then
        uv venv --python 3.13 "$PYODIDE_VENV"
    else
        python3.13 -m venv "$PYODIDE_VENV"
    fi
fi
if [[ "$("$PYODIDE_VENV/bin/python" -c 'import importlib.metadata as m; print(m.version("pyodide-build"))' 2>/dev/null)" != "$PYODIDE_BUILD_VERSION" ]]; then
    echo "==> installing pyodide-build $PYODIDE_BUILD_VERSION into $PYODIDE_VENV"
    if command -v uv >/dev/null; then
        VIRTUAL_ENV="$PYODIDE_VENV" uv pip install "pyodide-build==$PYODIDE_BUILD_VERSION"
    else
        "$PYODIDE_VENV/bin/pip" install "pyodide-build==$PYODIDE_BUILD_VERSION"
    fi
fi
# shellcheck disable=SC1091
source "$PYODIDE_VENV/bin/activate"

if ! pyodide xbuildenv versions 2>/dev/null | grep -q "$PYODIDE_VERSION"; then
    echo "==> installing Pyodide $PYODIDE_VERSION cross-build environment"
    pyodide xbuildenv install "$PYODIDE_VERSION"
fi
pyodide xbuildenv use "$PYODIDE_VERSION" >/dev/null

EMSCRIPTEN_VERSION="$(pyodide config get emscripten_version)"
if [[ ! -x "$EMSDK_DIR/emsdk" ]]; then
    echo "==> cloning emsdk into $EMSDK_DIR"
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi
"$EMSDK_DIR/emsdk" install "$EMSCRIPTEN_VERSION" >/dev/null
"$EMSDK_DIR/emsdk" activate "$EMSCRIPTEN_VERSION" >/dev/null
# shellcheck disable=SC1091
EMSDK_QUIET=1 source "$EMSDK_DIR/emsdk_env.sh"

echo "==> Pyodide $PYODIDE_VERSION, emscripten $(emcc -dumpversion)"
mkdir -p "$OUTDIR"
cd "$ROOT"
pyodide build --outdir "$OUTDIR" "$ROOT"
ls -l "$OUTDIR"/tttrlib-*pyodide*_wasm32.whl
