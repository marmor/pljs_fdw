# pljs_fdw Development

pljs_fdw is a PostgreSQL Foreign Data Wrapper that lets FDW authors implement
foreign tables as JavaScript classes. It's a single-file `C` extension
(`fdw.c`) built with PGXS, embedding its own copy of
[QuickJS](https://bellard.org/quickjs/quickjs.html) independent of
[PLJS](https://github.com/plv8/pljs)'s.

## Relationship to PLJS

pljs_fdw is a separate PGXS extension from PLJS, in a separate repository,
but it depends on PLJS in two ways:

- **Install-time**: `pljs_fdw.control` declares `requires = 'pljs'`, so
  `CREATE EXTENSION pljs_fdw` refuses to run until `CREATE EXTENSION pljs`
  already has. Foreign table modules are stored in and loaded from PLJS's own
  `pljs.modules` table (the same table `pljs.require()` reads from).
- **Runtime, not build-time**: `fdw.c` calls six PLJS functions
  (`pljs_setup_namespace`, `pljs_module_require`, `pljs_datum_to_jsvalue`,
  `pljs_jsvalue_to_datums`, `pljs_js_array_length`, `pljs_dump_error`),
  resolved from `pljs.so` at runtime via Postgres's own
  `load_external_function()`/`lookup_external_function()` (see
  [ARCHITECTURE.md](ARCHITECTURE.md) for why runtime resolution rather than
  linking against `pljs.so` at build time). `fdw.c` has no `#include` of
  PLJS's own header and no PLJS source tree needs to be present to build
  pljs_fdw at all.

This means the `pljs.so` pljs_fdw is installed against needs two changes that
aren't (yet) in every PLJS release:

1. `PGDLLEXPORT` markers on the six functions above, in PLJS's `src/pljs.h`
   (a linkage-only change — no signature or behavior change).
2. A one-line fix in PLJS's `src/types.c`: `pljs_jsvalue_to_datum()`'s
   NULL-`fcinfo` branch called `PG_RETURN_NULL()`, which unconditionally
   dereferences `fcinfo` — exactly the pointer that branch exists to handle
   being NULL. pljs_fdw's own calls into that function (with `fcinfo = NULL`,
   outside any SQL function call) depend on this fix to not crash.

Both are small, independently-justifiable changes proposed upstream to PLJS;
until/unless merged, apply them to whatever PLJS checkout you're building
`pljs.so` from.

## Building

```bash
make
make install
```

First build clones the `deps/quickjs` git submodule and applies the two
patches in `patches/` (forcing `-fPIC`, and renaming
`unicode_to_utf8`/`unicode_from_utf8` to avoid a link-time symbol clash —
both generic requirements of embedding QuickJS in a Postgres `.so`, not
pljs_fdw-specific); subsequent builds just recompile `fdw.c`. `pg_config`
must be on `PATH` (or set `PG_CONFIG=/path/to/pg_config`).

Build flags: `DEBUG=1` adds `-g`; `DEBUG_MEMORY=1` builds with ASan.

## Installing

```sql
CREATE EXTENSION pljs;      -- must come first
CREATE EXTENSION pljs_fdw;
```

or, to install both at once: `CREATE EXTENSION pljs_fdw CASCADE;`.

## Testing

No automated regression suite yet — verification so far has been manual,
through `psql`, exercising each FdwRoutine callback with real JS modules
(see [FDW_API.md](FDW_API.md) for the class contract). Adding `sql`/`expected`
regression tests, matching PLJS's own `pg_regress`-based convention, is on
the list of things worth doing next.

## File Naming Conventions

- `fdw.c` — the entire extension. Deliberately kept in one file, matching the
  isolation goal: a single, self-contained diff to review, drop in, or pull
  out (see [ARCHITECTURE.md](ARCHITECTURE.md)). Section header comments
  (`/* ---------- Section Name ---------- */`) divide it by concern (JS
  namespace, context cache, qual/sort pushdown, column pruning/mapping, scan
  execution, write support, direct modify, TRUNCATE, validator, IMPORT
  FOREIGN SCHEMA).
- `pljs_fdw.control` / `pljs_fdw.sql` — extension metadata and the SQL script
  registering the handler/validator functions and the `pljs_fdw` foreign-data
  wrapper.
- `deps/quickjs` — vendored QuickJS (git submodule), `patches/` — the two
  patches applied to it.

## Function Naming Conventions

All non-`static` functions and every function local to this file are
prefixed `pljs_fdw_`, distinguishing them from PLJS's own `pljs_`-prefixed
functions this file calls into.
