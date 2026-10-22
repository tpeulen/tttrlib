"""A .pto should run on the machine it lands on.

`tttr pto exec FILE` makes a container self-executing: it prepends a 1024-byte
stub to the EBML document, which every reader already locates by scanning
(`File::find_ebml_header`), so the stub is free bytes to the format. The stub
is a POSIX sh script first -- `#!/bin/sh` on line one, so the kernel itself
runs the file on macOS and Linux -- and a batch file when renamed to `.cmd` on
Windows. Run directly, the file finds a reader (`tttr` first, because tttrlib
is what a machine usually has, then the standalone `pto`/`ptoview`), hands it
itself and the arguments.

What is tested here is the contract that makes that safe:

* the container is **byte-identical behind the stub** -- nothing was rewritten,
  so every reader opens the executable exactly as it opened the plain file;
* the stub is **idempotent** -- running `exec` twice replaces, never stacks;
* running the file **does the reading** -- it lists the container it carries,
  and an argument becomes the subcommand (`run.pto extract DIR` extracts);
* a machine with **no reader** still gets an answer: what the file is, how to
  open it, exit 69 rather than a cryptic not-found;
* the executable stays a **writable container** -- tttrlib opens it for update
  and the result still carries the stub.
"""
import os
import shutil
import subprocess

import pytest

import tttrlib


def _tttr_bin():
    """The tttr CLI, when one is built or installed.

    A source checkout needs a build first (`cmake --preset lib-only` then
    `cmake --build build-cli --target tttr`); point TTTR_BIN at one, or have
    it on PATH. Without either these tests skip rather than fail on a binary
    that was never going to be there.
    """
    env = os.environ.get("TTTR_BIN")
    if env and os.path.isfile(env) and os.access(env, os.X_OK):
        return env
    return shutil.which("tttr")


TTTR = _tttr_bin()
needs_cli = pytest.mark.skipif(TTTR is None, reason="no tttr CLI built or installed")
if TTTR is None:  # keep every subprocess call type-clean for checkers
    TTTR = "tttr"

#: The stub `tttr pto exec` prepends: 1024 bytes, shebang first.
STUB = 1024


def _container(path, title="Exec Test"):
    f = tttrlib.PtoFile()
    assert f.create(str(path), title), f.error()
    uid = f.add("photon-stream", "raw", "stream1", b"photon bytes 0123456789")
    assert uid > 0
    assert f.commit(), f.error()
    f.close()
    return uid


@needs_cli
def test_exec_makes_the_file_runnable_and_the_reader_agrees(tmp_path):
    p = tmp_path / "run.pto"
    _container(p)
    plain = p.read_bytes()

    r = subprocess.run([TTTR, "pto", "exec", str(p)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr

    # The executable bit, on the platforms that have one.
    if os.name == "posix":
        assert os.access(p, os.X_OK)

    # Shebang on line one, so the kernel -- not a shell fallback -- runs it.
    out = p.read_bytes()
    assert out.startswith(b"#!/bin/sh\n")
    assert len(out) == STUB + len(plain)
    assert out[STUB:] == plain  # byte-identical behind the stub

    # Every reader opens the executable exactly as it opened the plain file.
    assert tttrlib.is_pto_file(str(p))
    f = tttrlib.PtoFile()
    assert f.open(str(p), False), f.error()
    assert f.title() == "Exec Test"
    assert f.ebml_offset() == STUB
    f.close()


@needs_cli
def test_exec_is_idempotent(tmp_path):
    p = tmp_path / "run.pto"
    _container(p)
    assert subprocess.run([TTTR, "pto", "exec", str(p)]).returncode == 0
    once = p.read_bytes()
    assert subprocess.run([TTTR, "pto", "exec", str(p)]).returncode == 0
    assert p.read_bytes() == once


@needs_cli
def test_running_the_file_lists_and_extracts(tmp_path):
    p = tmp_path / "run.pto"
    _container(p, title="Self Runner")
    assert subprocess.run([TTTR, "pto", "exec", str(p)]).returncode == 0

    # Direct execution: the stub finds this very tttr and hands it the file.
    r = subprocess.run([str(p)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "stream1" in r.stdout

    # An argument becomes the subcommand: info names the title.
    r = subprocess.run([str(p), "info"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "Self Runner" in r.stdout

    # extract: the argument lands behind the file, as `tttr pto extract` takes it.
    out = tmp_path / "unpacked"
    out.mkdir()
    r = subprocess.run([str(p), "extract", str(out)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    got = out / "stream1"
    assert got.is_file()
    assert got.read_bytes() == b"photon bytes 0123456789"


@needs_cli
def test_a_machine_without_a_reader_gets_an_answer(tmp_path):
    p = tmp_path / "run.pto"
    _container(p, title="Stranded")
    assert subprocess.run([TTTR, "pto", "exec", str(p)]).returncode == 0

    # A PATH holding only what the system itself needs: /usr/bin:/bin has no
    # tttr, pto or ptoview, so the stub must explain itself and exit 69.
    r = subprocess.run(["/bin/sh", str(p)], capture_output=True, text=True,
                       env={"PATH": "/usr/bin:/bin"})
    assert r.returncode == 69
    assert "PTO photon container" in r.stderr
    assert "tttr pto extract" in r.stderr


@needs_cli
def test_the_executable_stays_a_writable_container(tmp_path):
    p = tmp_path / "run.pto"
    _container(p)
    assert subprocess.run([TTTR, "pto", "exec", str(p)]).returncode == 0

    f = tttrlib.PtoFile()
    assert f.open(str(p), True), f.error()
    assert f.add("note", "text", "late", b"added after exec") > 0
    assert f.commit(), f.error()
    f.close()

    # Still executable, still readable, still carrying its payload.
    assert os.access(p, os.X_OK)
    f = tttrlib.PtoFile()
    assert f.open(str(p), False), f.error()
    assert f.find("late") > 0
    f.close()


@needs_cli
def test_exec_refuses_something_that_is_not_a_container(tmp_path):
    junk = tmp_path / "junk.pto"
    junk.write_bytes(b"not a container at all")
    r = subprocess.run([TTTR, "pto", "exec", str(junk)], capture_output=True, text=True)
    assert r.returncode != 0
    assert "not a" in (r.stderr + r.stdout).lower()
