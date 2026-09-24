# SPDX-License-Identifier: BSD-3-Clause
# The PTO.MFDB profile over PtoFile: typed tag lookup, lineage edges, verified
# payloads, the artifact/operation/edge description, the one-writer lock, and
# the legacy .bur row interleave. Appended to Pto.i's Python code, so every
# reader of a ChiSurf/tttrlib measurement container (ChiSurf, ndXplorer, the
# CLI's Python callers) shares one reading of the tags instead of one each.
#
# The tag names ARE mmCIF item names from the MMFDB dictionary. Validating a
# term against the dictionary is the writer's business (it needs the
# dictionary); nothing here does, so this layer works without it.

#: mmCIF items this layer reads and writes.
PTO_MFDB_ARTIFACT_ID = "_mmfdb_artifact.artifact_id"
PTO_MFDB_DATA_FORMAT = "_mmfdb_artifact.data_format"
PTO_MFDB_ROW_GRAIN = "_mmfdb_artifact.row_grain"
PTO_MFDB_CHECKSUM = "_mmfdb_artifact.checksum"
PTO_MFDB_OPERATION_TYPE = "_mmfdb_operation.operation_type"
PTO_MFDB_SETTINGS_JSON = "_mmfdb_operation.settings_json"
PTO_MFDB_SETTINGS_HASH = "_mmfdb_operation.settings_hash"
PTO_MFDB_SOURCE_NODE_ID = "_mmfdb_edge.source_node_id"
PTO_MFDB_RELATIONSHIP_TYPE = "_mmfdb_edge.relationship_type"


def pto_tag(file, uid, name, default=""):
    """The value of tag *name* on object *uid* (``0``: the file), or *default*.

    The first tag of that name wins. Text, unsigned, UID, signed and float tags
    come back as ``str``, ``int`` or ``float``; any other type as *default*.
    """
    for t in file.tags_for(int(uid)):
        if t.name != name:
            continue
        if t.type == PtoType_Text:
            return t.text
        if t.type in (PtoType_UInt, PtoType_UID):
            return t.u
        if t.type == PtoType_Int:
            return t.i
        if t.type == PtoType_Float:
            return t.d
        return default
    return default


def pto_parents(file, uid):
    """UIDs object *uid* was derived from (its ``_mmfdb_edge.source_node_id`` tags).

    A UID-typed ``relationship_type`` tag counts too: containers written before
    the parent and the relation were two tags carried the parent under the
    relation's name.
    """
    return [t.u for t in file.tags_for(int(uid))
            if t.type == PtoType_UID
            and t.name in (PTO_MFDB_SOURCE_NODE_ID, PTO_MFDB_RELATIONSHIP_TYPE)]


def pto_read_blob(file, uid, verify=True):
    """An object's payload as ``bytes``, checked against its recorded SHA-256.

    :raises RuntimeError: when the object cannot be read, or (with *verify*)
        its bytes do not match ``_mmfdb_artifact.checksum``.
    """
    import hashlib
    data = file.read(int(uid))
    if data is None:
        raise RuntimeError(f"could not read object {uid}: {file.error()}")
    data = bytes(data)
    recorded = pto_tag(file, uid, PTO_MFDB_CHECKSUM) if verify else ""
    if recorded:
        actual = hashlib.sha256(data).hexdigest()
        if actual != recorded:
            raise RuntimeError(f"object {uid} does not match its recorded checksum "
                               f"({recorded[:16]}... expected, {actual[:16]}... found)")
    return data


def pto_settings_hash(settings):
    """The run identity of an operation: SHA-256 of its settings as sorted JSON."""
    import hashlib
    import json
    blob = json.dumps(dict(settings or {}), sort_keys=True, separators=(",", ":"), default=str)
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


def _pto_mfdb_put(file, uid, name, value, kind):
    tag = PtoTag()
    tag.name, tag.type, tag.target = name, kind, int(uid)
    if kind == PtoType_Text:
        tag.text = str(value)
    else:
        tag.u = int(value)
    file.add_tag(tag)


def pto_describe(file, uid, data_format="", row_grain="", checksum="", size_bytes=0,
                 mime_type="", file_path="", operation_type="", algorithm="",
                 parameters=None, run="", derived_from=(), relationship_type="derived_from",
                 source_row_column="", target_row_column="", software="",
                 dictionary_version="", dictionary_hash=""):
    """Attach the artifact, operation and edge rows object *uid* stands for.

    Empty values are not written. An ``artifact_id`` is minted once. A parent
    already recorded is not recorded again: a re-run updates an object in
    place but tags append, and an edge written twice is not truer.

    :param software: ``"<package> <version>"`` of the writer, recorded with the
        operation.
    :param derived_from: parent UIDs (an int or a sequence of them).
    """
    import json
    import uuid as _uuid

    def text(item, value):
        if value:
            _pto_mfdb_put(file, uid, item, value, PtoType_Text)

    if not pto_tag(file, uid, PTO_MFDB_ARTIFACT_ID):
        text(PTO_MFDB_ARTIFACT_ID, str(_uuid.uuid4()))
    text(PTO_MFDB_DATA_FORMAT, data_format)
    text(PTO_MFDB_ROW_GRAIN, row_grain)
    text("_mmfdb_artifact.mime_type", mime_type)
    text("_mmfdb_artifact.file_path", file_path)
    if checksum:
        text(PTO_MFDB_CHECKSUM, checksum)
        text("_mmfdb_artifact.checksum_algorithm", "sha256")
    if size_bytes:
        _pto_mfdb_put(file, uid, "_mmfdb_artifact.size_bytes", size_bytes, PtoType_UInt)
    if operation_type:
        package, _, version = str(software).partition(" ")
        text(PTO_MFDB_OPERATION_TYPE, operation_type)
        text("_mmfdb_operation.algorithm", algorithm)
        text(PTO_MFDB_SETTINGS_HASH, run)
        text("_mmfdb_operation.software_package", package)
        text("_mmfdb_operation.software_version", version.strip() or package)
        text("_mmfdb_operation.dictionary_version", dictionary_version)
        text("_mmfdb_operation.dictionary_hash", dictionary_hash)
        if parameters:
            text(PTO_MFDB_SETTINGS_JSON, json.dumps(dict(parameters), sort_keys=True, default=str))
    parents = [int(derived_from)] if isinstance(derived_from, int) else \
        [int(p) for p in (derived_from or ()) if p]
    recorded = set(pto_parents(file, uid))
    for parent in parents:
        if parent and parent not in recorded:
            recorded.add(parent)
            _pto_mfdb_put(file, uid, PTO_MFDB_SOURCE_NODE_ID, parent, PtoType_UID)
    if parents:
        text(PTO_MFDB_RELATIONSHIP_TYPE, relationship_type)
    text("_mmfdb_edge.source_row_column", source_row_column)
    text("_mmfdb_edge.target_row_column", target_row_column)


def pto_add_blob(file, kind, data_format, name, data, operation_type="", parameters=None,
                 **describe):
    """Write an opaque payload and describe it (checksum and size included).

    The run of an operation defaults to :func:`pto_settings_hash` of its
    *parameters*. Nothing is committed.

    :returns: the object UID.
    :raises RuntimeError: when the container refuses the write.
    """
    import hashlib
    payload = bytes(data)
    uid = file.add(kind, data_format, name, payload)
    if not uid:
        raise RuntimeError(f"could not write {name}: {file.error()}")
    if operation_type:
        describe.setdefault("run", pto_settings_hash(parameters))
    pto_describe(file, uid, data_format=data_format, checksum=hashlib.sha256(payload).hexdigest(),
                 size_bytes=len(payload), operation_type=operation_type,
                 parameters=parameters, **describe)
    return uid


class PtoLockedError(RuntimeError):
    """Another process holds the container open for writing."""


class PtoWriteLock:
    """One writer per container, across processes: an advisory ``flock``.

    On a ``<container>.lock`` sidecar rather than the container, which the
    writer replaces underneath. Fails at once instead of waiting -- a writer
    that blocks looks like one that hung. Best effort where there is nothing to
    lock with (no ``fcntl``, an unwritable directory, a filesystem without
    ``flock`` such as a browser's): the write goes ahead unlocked.
    """

    SUFFIX = ".lock"

    def __init__(self, path):
        self.path = str(path) + self.SUFFIX
        self._handle = None

    def acquire(self):
        """Take the lock. :raises PtoLockedError: if another process has it."""
        import errno
        import os
        try:
            import fcntl
        except ImportError:
            return self
        try:
            handle = open(self.path, "a+")
        except OSError:
            return self
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK, errno.EACCES):
                handle.close()
                return self
            handle.seek(0)
            holder = handle.read().strip() or "another process"
            handle.close()
            raise PtoLockedError(
                f"{self.path[:-len(self.SUFFIX)]} is open for writing by {holder}. Close it "
                "there, or wait for that write to finish: two writers would each "
                "overwrite the other's results.") from None
        handle.seek(0)
        handle.truncate()
        handle.write(f"pid {os.getpid()}")
        handle.flush()
        self._handle = handle
        return self

    def release(self):
        """Drop the lock and remove the sidecar."""
        import contextlib
        import os
        handle, self._handle = self._handle, None
        if handle is None:
            return
        with contextlib.suppress(Exception):
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        with contextlib.suppress(OSError):
            handle.close()
        with contextlib.suppress(OSError):
            os.remove(self.path)

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self.release()
        return False


def deinterleave_burst_rows(store):
    """A burst table without the legacy ``.bur`` interleave and blank columns.

    The ``.bur`` text format writes ``2N+1`` rows -- a zero row, a burst, a zero
    row, ... -- so companions can be merged by position, and a trailing unnamed
    column for the header's trailing tab. Containers written from such tables
    carry both. A table whose even rows are not all zero in every numeric column
    that has a finite value is returned as it is (a copy, when a blank column
    is dropped), so this is safe on any table.
    """
    import numpy as np
    out = store
    n = int(store.n_rows())
    if n >= 3 and n % 2 == 1:
        numeric = []
        for i in range(store.n_columns()):
            column = store.column(i)
            if not column.is_numeric():
                continue
            values = np.asarray(column.numpy(), dtype=float)
            if np.isfinite(values).any():
                numeric.append(values)
        if numeric and all(np.all(v[0::2] == 0) for v in numeric):
            out = store.take(np.arange(1, n, 2, dtype=np.int64))
    blank = [i for i in range(out.n_columns()) if not str(out.column(i).name()).strip()]
    if blank:
        out = out.copy() if out is store else out
        for i in reversed(blank):
            out.remove_column(i)
    return out
