# Architecture

## Own JSRuntime, own QuickJS

pljs_fdw runs its own `JSRuntime` (created once, in a constructor-attribute
function that runs when `pljs_fdw.so` is loaded), entirely independent of
PLJS's own global `rt`. It vendors its own copy of QuickJS
(`deps/quickjs`, pinned to the same upstream commit PLJS itself uses) rather
than sharing PLJS's — the two never share any QuickJS state, so there's no
reason for them to be the same build, only reasonably close versions.

## `pljsFdwNet` and JS-level libraries via `pljs.require()`

`pljsFdwNet` (raw TCP sockets plus `sha256`/`hmacSha256`, added in `net.c`)
is the one deliberately generic capability in the JS namespace, aimed at any
FDW module that needs to talk to a networked service — not just the
Postgres wire-protocol client (`examples/pgwire.js`) it was built for. It's
blocking, matching this framework's synchronous `execute()` model; no
async I/O primitive exists here, on purpose (see "Deliberately not
implemented" below).

Two real, general `pljs.require()`/`pljs_module_require()` quirks turned up
building `pgwire.js`/`examples/remote_pg_fdw.js` on top of it, worth
knowing before writing any FDW module with a dependency on a shared
`pljs.modules` library:

- **It doesn't cache, and evaluates into the shared global scope.**
  `module`/`exports` are plain global properties set fresh on every call,
  not a per-file closure the way Node's `require()` works. A second
  `pljs.require('somelib')` anywhere in the same JSContext — which will
  happen, e.g. two foreign tables both using the same client library — hits
  a `class`/`const` redeclaration error if the required module has
  top-level declarations. Fix: wrap the whole module body in an IIFE that
  returns what `module.exports` should be, so nothing leaks into global
  scope across repeated evaluations (see `pgwire.js`'s header comment).
- **It can't be called while another module's own top-level evaluation is
  still running.** Calling `pljs.require()` at the top level of a module
  that is *itself* mid-load (i.e. nested inside another
  `pljs_module_require()`/`pljs.require()` call already on the stack)
  breaks silently — not even a catchable JS exception at the call site,
  just a load that never properly finishes, surfacing far upstream as
  `fdw.c`'s generic "unable to instantiate FDW class" / "not a constructor"
  error. Fix: defer the `require()` call to actual runtime — e.g. inside a
  constructor, called later once the requiring module's own load has fully
  completed — rather than at that module's top level (see
  `remote_pg_fdw.js`'s constructor).

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

**A porting hazard worth calling out explicitly**: `pljs_fdw`'s qual
recognizer (`pljs_fdw_extract_pushable_quals`, `fdw.c`) is a purely
syntactic, always-on pattern match over the query's `WHERE` clause — there
is no way for a JS module to opt out of receiving a qual it doesn't want to
handle. This matters because Multicorn's own FDWs are built against a
*different* default: several of them (`csvfdw`, `xmlfdw`, and likely most
others that don't explicitly implement pushdown) take a `quals` argument
and simply never look at it, relying on Multicorn/Postgres to recheck every
row locally afterward — safe under Multicorn's model, not under this one.
A direct port of that behavior here doesn't degrade gracefully into "just a
missed optimization" the way it would upstream: it silently returns every
row regardless of the query's `WHERE` clause, because `pljs_fdw` skips its
own recheck for whatever it recognizes and offers, trusting `execute()` to
have honored it. This was caught by direct testing during the `csv_fdw`
port (`WHERE id = 3` initially returned every row, unfiltered) — not
something that would show up from reading the Python source alone. **Any
module ported from a Multicorn FDW that doesn't itself implement real
pushdown must still filter by `quals` in `execute()`** (and, if it sorts,
by `sortkeys` too) purely to preserve correctness under this framework's
contract, independent of whether the original bothered to.

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
