// fs_fdw: a read-only FDW listing files under a local directory tree, with
// each file's content, as rows. A deliberately simplified port of the base
// of Multicorn's fsfdw (structuredfs.py's FilesystemFdw): the original is
// a full pattern-based file store -- path templates like
// '{category}/{number}_{name}.txt' compiled into per-segment matchers,
// full read/write/delete with POSIX file locking, and a whole
// transaction-buffered pre_commit()/rollback() write pipeline layered on
// Multicorn's own transaction-lifecycle hooks -- substantially more than a
// first port at this scope should take on. Deliberately dropped, not just
// casually deferred:
//   - Path-pattern templating: this module lists a plain directory tree
//     instead, using each file's own relative path as its identity.
//   - Write/delete/locking: pljsFdwFs currently only has read-only
//     primitives (readDir/readFile/readLink) -- no writeFile/unlink/mkdir/
//     flock exist yet. Adding those, and a write path built on them, is
//     real future work, not something this module papers over.
//   - Transaction-buffered writes: pljs_fdw has no begin/commit/rollback
//     hooks at all (see docs/ARCHITECTURE.md's "Deliberately not
//     implemented" section) -- there's no framework equivalent to defer
//     to, so any future write support here would write immediately, like
//     every other write-capable FDW in this project (remote_pg_fdw.js,
//     self_test_fdw.js), not buffer until commit.
// Adding insert/update/delete later is expected to be additive on top of
// this same row model (one row per file, keyed by its relative `name`),
// not a rewrite -- see the design discussion this was built from.
//
// Options:
//   rootDir   -- directory to list (required)
//   recursive -- 'false' to only list rootDir's direct entries (default:
//                recurse into subdirectories, matching the spirit of the
//                original's multi-segment path patterns)
//
// Columns: `name` (the file's path relative to rootDir, forward-slash
// separated regardless of platform) and `content` (the file's full text
// content, or SQL NULL if it can't be read as a file -- e.g. it's a
// directory, a special file, or unreadable).
//
// A note on telling files apart from directories: pljsFdwFs has no
// stat()-equivalent at all (see the "dropped" list above), so this module
// distinguishes them by trying pljsFdwFs.readDir() on each entry and
// falling back to treating it as a file if that throws (ENOTDIR) -- a
// deliberate reuse of an existing primitive's success/failure as an
// implicit type check, not a new native capability. This does mean a
// directory's contents get listed twice (once to test, once to actually
// walk it) -- an acceptable inefficiency for a first port, not a
// correctness issue.
module.exports = (function() {

function toNumIfNumeric(v) {
  if (typeof v === 'number') return v;
  if (typeof v === 'string' && v.trim() !== '' && !isNaN(v)) return Number(v);
  return v;
}

function evalQual(row, qual) {
  const rawV = toNumIfNumeric(row[qual.column]);
  const rawT = toNumIfNumeric(qual.value);
  const bothNumeric = typeof rawV === 'number' && typeof rawT === 'number';
  const v = bothNumeric ? rawV : String(rawV);
  const t = bothNumeric ? rawT : String(rawT);

  switch (qual.operator) {
    case '=': return v === t;
    case '<>': return v !== t;
    case '<': return v < t;
    case '<=': return v <= t;
    case '>': return v > t;
    case '>=': return v >= t;
    default: return true;
  }
}

function isDirectory(path) {
  try {
    pljsFdwFs.readDir(path);
    return true;
  } catch (e) {
    return false;
  }
}

/** @brief Yields each regular file's path, relative to @p baseDir, under
 * @p baseDir + '/' + relPath (relPath is '' at the top of the walk). */
function* walk(baseDir, relPath, recursive) {
  const full = relPath ? baseDir + '/' + relPath : baseDir;
  const entries = pljsFdwFs.readDir(full);

  for (const entry of entries) {
    const entryRel = relPath ? relPath + '/' + entry : entry;
    const entryFull = baseDir + '/' + entryRel;

    if (recursive && isDirectory(entryFull)) {
      yield* walk(baseDir, entryRel, recursive);
    } else if (!isDirectory(entryFull)) {
      yield entryRel;
    }
    // A non-recursive directory entry (recursive === false) is silently
    // skipped, matching "list this directory's files" rather than
    // erroring on a subdirectory it wasn't asked to descend into.
  }
}

class FsFdw {
  constructor(options) {
    if (!options.rootDir) {
      throw new Error('fs_fdw: "rootDir" option is required');
    }

    this.rootDir = options.rootDir;
    this.recursive = options.recursive !== 'false';
  }

  *execute(quals, columns) {
    const wantContent = columns.indexOf('content') !== -1;

    for (const name of walk(this.rootDir, '', this.recursive)) {
      const row = {name};

      if (wantContent) {
        try {
          row.content = pljsFdwFs.readFile(this.rootDir + '/' + name);
        } catch (e) {
          row.content = null;
        }
      }

      if (!quals || quals.every((q) => evalQual(row, q))) {
        yield row;
      }
    }
  }
}

return FsFdw;

})();
