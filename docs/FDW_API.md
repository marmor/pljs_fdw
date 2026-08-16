# Writing a Foreign Data Wrapper Module

A pljs_fdw-backed foreign table is implemented as a CommonJS module, stored
in PLJS's `pljs.modules` table (the same table `pljs.require()` reads from)
and named by a `module` option on the foreign table or its server. Its
`module.exports` is a class:

```js
module.exports = class {
  constructor(options) { ... }                          // merged server+table options
  estimate(quals, columns, columnMap) { return N; }      // optional; row estimate
  *execute(quals, columns, sortkeys, columnMap) { ... }  // generator, yields row objects
  insert(row) { ... }                                    // optional; enables INSERT + COPY FROM
  insertMany(rows) { ... }                               // optional; enables batched INSERT/COPY
  get rowIdColumn() { return 'id'; }                     // required for update()/delete()
  update(rowId, row) { ... }                             // optional; enables per-row UPDATE
  delete(rowId) { ... }                                  // optional; enables per-row DELETE
  updateWhere(quals, values) { return N; }                // optional; enables direct UPDATE
  deleteWhere(quals) { return N; }                        // optional; enables direct DELETE
  truncate({ cascade, restartSequences }) { ... }        // optional; enables TRUNCATE
};
```

Only `execute()` is required. Everything else is opt-in — a module that
defines none of the write/TRUNCATE methods backs a plain read-only foreign
table.

The class is instantiated once per `(role, foreign table)` pair and cached
for the life of the backend (see [ARCHITECTURE.md](ARCHITECTURE.md)), so the
constructor is a good place for one-time setup.

## Reading: `execute()`

```js
*execute(quals, columns, sortkeys, columnMap) {
  for (const row of mySource) {
    yield row;
  }
}
```

A generator (or any iterator) yielding plain objects, one per row, keyed by
the foreign table's local Postgres column names. `quals`, `columns`, and
`sortkeys` are all **pushed down for real**: if `execute()` is offered a
qual or sort key, Postgres trusts it was honored and does not re-check
locally (no recheck qual, no `Sort` node). Silently ignoring one of these
arguments produces silently wrong query results, not just a missed
optimization — this is a correctness contract, the same as any real FDW's
pushdown.

### `quals`

An array of simple `column <op> constant` restrictions recognized from the
query's `WHERE` clause (and their commuted form, `constant <op> column`):

```js
[{ column: 'status', operator: '=', value: 'active' }, ...]
```

Supported operators: `=`, `<>`, `<`, `<=`, `>`, `>=`. Anything more complex
(expressions, non-constant comparisons, unsupported operators) is left for
Postgres to filter locally instead — `execute()` only ever receives quals it
must apply, never ones it's merely free to apply.

### `columns`

An array of the local column names actually needed for this scan (referenced
in the target list or in any restriction clause) — real column pruning, not
just a hint:

```js
['id', 'name']
```

A whole-row reference (`SELECT foreign_table FROM foreign_table`) resolves
to every column.

### `sortkeys`

The longest leading prefix of the query's `ORDER BY` that maps to plain
columns with default comparison semantics:

```js
[{ column: 'created_at', direction: 'desc', nullsFirst: false }, ...]
```

If given a non-empty `sortkeys`, `execute()` must yield rows in that exact
order — Postgres skips adding an explicit `Sort` node when it believes this
was pushed down.

### `columnMap`

An object covering only the columns whose remote name differs from the local
Postgres column name (set via `ALTER FOREIGN TABLE ... ALTER COLUMN col
OPTIONS (column_name '...')`, the same option name `postgres_fdw` uses):

```js
{ id: 'cust_id', name: 'cust_name' }
```

Purely informational — rows are still always keyed by the **local** column
name regardless of `columnMap`; it just tells `execute()` what to call each
field on the remote side, without hardcoding that translation.

## `estimate()`

```js
estimate(quals, columns, columnMap) {
  return 1000; // approximate row count
}
```

Optional. Returning a realistic row estimate helps the planner make better
join-order and path-cost decisions. Defaults to a fixed 1000 rows if
omitted, or if `estimate()` returns something less than 1.

## Writing

### `insert(row)` / `insertMany(rows)`

```js
insert(row) {
  const id = mySource.insert(row);
  return { id };  // optional: reflect server-computed columns back
}
```

`insert()` alone is enough for `INSERT INTO foreign_table ...` and `COPY
foreign_table FROM ...` support — there's no existing row to identify.
Returning an object overwrites the inserted row's values (e.g. a
server-generated id); returning nothing leaves the given values as-is.

`insertMany(rows)` additionally enables batching: a multi-row `INSERT` or
`COPY FROM` calls it once with up to 100 rows instead of calling `insert()`
once per row. Not used when the statement has `RETURNING`, a `WITH CHECK
OPTION`, or a `BEFORE`/`AFTER ROW INSERT` trigger — those fall back to
`insert()`, one row at a time.

### `update(rowId, row)` / `delete(rowId)`

```js
get rowIdColumn() { return 'id'; }

update(rowId, row) { mySource.update(rowId, row); }
delete(rowId) { mySource.delete(rowId); }
```

Per-row UPDATE/DELETE. Both require `rowIdColumn`, naming an existing column
Postgres re-fetches per affected row and passes as `rowId`. Like `insert()`,
returning an object from `update()` overwrites the row's values.

### `updateWhere(quals, values)` / `deleteWhere(quals)`

```js
updateWhere(quals, values) {
  return mySource.updateWhere(quals, values); // return affected-row count
}
deleteWhere(quals) {
  return mySource.deleteWhere(quals);
}
```

"Direct modify": an alternative to the per-row path that performs a whole
`UPDATE`/`DELETE` in one call instead of scanning and calling `update()`/
`delete()` per row. Postgres only offers this when nothing would be lost by
skipping the per-row path — the `WHERE` clause is entirely pushable (same
`quals` shape as `execute()`), and for `UPDATE`, every `SET` assignment is a
plain constant (`values`: `{column: value, ...}`), with no `RETURNING` and
no `BEFORE`/`AFTER ROW` trigger involved. Otherwise Postgres falls back to
the per-row path automatically. Independent of `rowIdColumn` — a class can
offer either path, both, or neither.

### `truncate({ cascade, restartSequences })`

```js
truncate({ cascade, restartSequences }) {
  mySource.truncate();
}
```

Enables `TRUNCATE foreign_table`. `cascade` is true for `TRUNCATE ...
CASCADE`; `restartSequences` is true for `TRUNCATE ... RESTART IDENTITY`.

## Options

- **`module`** (server or table level; table overrides server) — required.
  The `pljs.modules` path to the module.
- **User mapping options** — merged into the constructor's `options`
  argument, typically per-user credentials (an API key, token, etc.). Not
  restricted to a fixed set — name them whatever your module needs.
- **`column_name`** (column level, via `ALTER FOREIGN TABLE ... ALTER
  COLUMN col OPTIONS (column_name '...')`) — see `columnMap` above.

```sql
CREATE SERVER my_srv FOREIGN DATA WRAPPER pljs_fdw
  OPTIONS (module 'my_fdw_module');

CREATE USER MAPPING FOR CURRENT_USER SERVER my_srv
  OPTIONS (api_key 'secret');

CREATE FOREIGN TABLE my_table (id int, name text)
  SERVER my_srv;
```

Server, user mapping, and table options are merged into one object passed to
the constructor — later ones win on a name collision (table overrides user
mapping overrides server).

## `IMPORT FOREIGN SCHEMA`

Since there's no natural remote catalog to query generically, `remote_schema`
in `IMPORT FOREIGN SCHEMA remote_schema FROM SERVER srv INTO local_schema` is
treated as a `pljs.modules` path to a *schema-describing* module — a
different shape than the per-table class above, since one `IMPORT` can
produce many tables at once:

```js
module.exports = function (options) {
  return {
    orders: {
      columns: { id: 'integer', total: 'numeric' },
      options: { module: 'orders_impl' },
    },
    customers: {
      columns: {
        id: 'integer',
        // an object instead of a plain string also sets column_name
        name: { type: 'text', name: 'cust_name' },
      },
      options: { module: 'customers_impl' },
    },
  };
};
```

`options` is `IMPORT`'s own `OPTIONS (...)` clause, if given. Each column's
`type` is emitted verbatim as a Postgres type name in the generated `CREATE
FOREIGN TABLE` statements; a bad type name surfaces as an ordinary DDL parse
error when Postgres executes them. `LIMIT TO (...)`/`EXCEPT (...)` are
honored against the returned table names.
