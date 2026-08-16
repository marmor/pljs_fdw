# Architecture

## Own JSRuntime, own QuickJS

pljs_fdw runs its own `JSRuntime` (created once, in a constructor-attribute
function that runs when `pljs_fdw.so` is loaded), entirely independent of
PLJS's own global `rt`. It vendors its own copy of QuickJS
(`deps/quickjs`, pinned to the same upstream commit PLJS itself uses) rather
than sharing PLJS's — the two never share any QuickJS state, so there's no
reason for them to be the same build, only reasonably close versions.

## Resolving PLJS functions at runtime, not build time

`fdw.c` calls six PLJS functions — the Datum↔JSValue marshaling and JS
namespace setup that would otherwise have to be duplicated. Rather than
linking `pljs_fdw.so` against `pljs.so` at build time, they're resolved
lazily, the first time any FDW operation runs, via Postgres's own
`load_external_function()`/`lookup_external_function()` (`fmgr.h`) — the
same mechanism Postgres itself uses to load an extension library.

This wasn't the first design tried. Linking directly against `pljs.so`
(`-l:pljs.so`, since it has no `lib` prefix) works and builds cleanly, but
breaks at runtime: Postgres's own extension loader (`dfmgr`) calls
`dlsym(handle, "_PG_init")` on every library it loads, to find that
library's init function. glibc's `dlsym` on a `dlopen()` handle also
searches that handle's `DT_NEEDED` dependencies — so it found `pljs.so`'s
`_PG_init` *through* the link dependency and called it a second time in the
same backend, hitting `attempt to redefine parameter "pljs.memory_limit"`.
Routing through `load_external_function()` instead uses the same
de-duplicated path Postgres itself always uses to load a library, so PLJS's
`_PG_init` only ever runs once, regardless of which extension's `.so` a
given backend happens to load first.

## Context/instance caching

The JSContext and instantiated class for a given `(role, foreign table)`
pair are cached across scans, so `BeginForeignScan` only pays the
module-load/construct cost once per backend per table. Only the per-scan
iterator `execute()` returns is scan-owned, freed in `EndForeignScan`; the
cached context/instance persist for the life of the backend.

The cache is invalidated two ways:

- `ALTER FOREIGN TABLE/SERVER/USER MAPPING ... OPTIONS` (e.g. changing
  `module`, or per-user credentials) flushes the whole cache, via a syscache
  invalidation callback checked at the top of `BeginForeignScan`.
- A source change to the specific cached module row (`UPDATE pljs.modules
  SET source = ...`) is detected per-entry via an xmin/ctid comparison and
  evicts just that entry, without needing a syscache callback on a plain
  data table.

## Pushdown philosophy: trust, not verify

Qual and sort pushdown (see [FDW_API.md](FDW_API.md)) are both real, not
hints: whatever `execute()` is offered, Postgres trusts was honored, and
does not re-check locally. This is deliberately conservative on the
*recognition* side — only simple `column <op> constant` restrictions (a
small allowlisted set of operators) and a leading prefix of `ORDER BY`
matching plain columns with default comparison semantics are ever offered.
Anything more complex is left for Postgres to handle locally, exactly as if
pushdown didn't exist for that clause. But once something *is* offered, it's
a correctness contract: an `execute()` that ignores a qual it was given
produces silently wrong results, not just a missed optimization.

Column pruning and column-name mapping follow the same trust model in
spirit, though with lower stakes — `columns` reflects real pruning (only
attributes actually needed for the scan), and `columnMap` is purely
informational (rows are still always keyed by the local column name).

## Direct modify vs. per-row writes

UPDATE/DELETE has two independent paths a module can opt into (see
[FDW_API.md](FDW_API.md)): per-row (`update()`/`delete()`, needing
`rowIdColumn`) and direct/set-based (`updateWhere()`/`deleteWhere()`,
needing nothing but a fully-pushable `WHERE`). Postgres only offers the
direct path when nothing would be lost by skipping the per-row scan
entirely — no `RETURNING`, no `BEFORE`/`AFTER ROW` trigger, and no local
recheck qual left un-pushed. Otherwise it falls back to the per-row path
automatically.

## Deliberately not implemented

- **DSM-based parallel scan** (`IsForeignScanParallelSafe`/
  `EstimateDSMForeignScan`/etc.). Even `postgres_fdw` — the reference
  FDW — implements none of these. Without an FDW building its own
  worker-coordination via those callbacks, marking a scan parallel-safe just
  means every worker in a `Gather` independently re-runs the whole scan and
  the results get unioned — every row silently duplicated once per worker.
  Doing this correctly would mean redesigning `execute()` to accept a worker
  index/count so the JS side partitions its own rows, not just wiring up a
  few callbacks.
- **Async execution** (`IsForeignPathAsyncCapable`/`ForeignAsyncRequest`/
  etc.). Unlike parallel scan, `postgres_fdw` genuinely uses this — but its
  whole point is overlapping wait time on a pollable resource (a libpq
  socket registered on the parent `Append` node's wait-event set).
  `execute()` is a synchronous QuickJS generator call with no pollable
  resource underneath at all; wiring up the async callbacks without a real
  async I/O primitive in the FDW's JS namespace would be pure ceremony —
  nothing to overlap, so no benefit, just added state-machine complexity.
  Revisit only if/when a genuine async I/O primitive (e.g. a non-blocking
  `fetch()` bridged to Postgres's wait-event mechanism) is built into the
  FDW namespace.
- **Batch UPDATE/DELETE** beyond direct modify. The `FdwRoutine` API itself
  only defines batching for INSERT (`ExecForeignBatchInsert`/
  `GetForeignModifyBatchSize`) — there's no equivalent for UPDATE/DELETE.
  `updateWhere()`/`deleteWhere()` (direct modify) already cover the
  equivalent case that's actually reachable: a fully pushable `WHERE`
  clause, performing the whole statement in one call rather than a per-row
  one. That's as far as batching UPDATE/DELETE goes here.
