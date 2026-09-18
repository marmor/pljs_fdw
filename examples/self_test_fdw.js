// self_test_fdw: a synthetic, config-driven FDW for exercising pljs_fdw's
// own contract end to end -- the pljs_fdw analog of Multicorn's
// testfdw.py/TestForeignDataWrapper, which exists purely to validate
// Multicorn's own framework internals rather than access any real external
// data source. This is NOT a port of a real integration; it's a
// diagnostic/regression tool for pljs_fdw development.
//
// Unlike testfdw.py, this drops:
//   - LIMIT/OFFSET pushdown (can_limit/limit/offset) -- pljs_fdw has no
//     such capability to test; fdw.c never sees a LIMIT clause at all.
//   - Transaction lifecycle hooks (begin/commit/rollback/sub_begin/etc.)
//     -- pljs_fdw exposes no equivalent callbacks.
//   - Positional ('sequence') row shape -- pljs_fdw's execute() always
//     yields plain {column: value} objects, never positional arrays.
//   - import_schema()'s bespoke fixture -- pljs_fdw's own IMPORT FOREIGN
//     SCHEMA contract (a schema-describing module, a different shape
//     entirely -- see docs/FDW_API.md) already has its own tests.
//
// What it keeps, adapted: a configurable synthetic row generator. But
// unlike testfdw.py, which just logs the quals/sortkeys it's given without
// checking them, this module actually FILTERS and SORTS its generated
// rows to match. That makes it self-verifying: since the "correct" answer
// is fully known ahead of time (it's synthetic data this module generated
// itself), a query against this table lets you compare what pljs_fdw
// actually returned against what the WHERE/ORDER BY *should* have
// produced. If fdw.c's qual/sort pushdown extraction ever regresses (wrong
// column, wrong operator, wrong direction), a query here returns rows
// that visibly don't match their own WHERE/ORDER BY -- a stronger check
// than trusting pushdown blindly.
//
// Options:
//   numRows     -- how many synthetic rows to generate (default 20)
//   mode        -- shape of each column's synthetic value (default 'int'):
//                  'int'      : the row index (0..numRows-1) -- numeric,
//                               for verifying numeric comparison/sort
//                  'string'   : "<column> <index>", e.g. "name 7"
//                  'date'     : a real JS Date object, one day per row --
//                               required for date/timestamp/timestamptz
//                               columns, since PLJS's Datum conversion for
//                               those types only recognizes an actual
//                               Date instance, not an ISO string (which
//                               silently becomes SQL NULL instead)
//                  'encoding' : a fixed non-ASCII string on every row
//                               (UTF-8 round-trip check)
//                  'null'     : every value is null (NULL-handling check)
//   rowIdColumn -- required for update()/delete() to work; no default,
//                  since (unlike testfdw.py) this module never sees the
//                  column list until execute()/estimate() are called
//   returning   -- 'true' to have insert()/update() echo back a modified
//                  copy of the given row (prefixed "INSERTED: "/
//                  "UPDATED: "), exercising the apply-returned-values-
//                  to-slot path (pljs_fdw_apply_jsvalue_to_slot). Only
//                  meaningful against `text`-typed local columns: PLJS
//                  converts a JS value to an int4 column via QuickJS's own
//                  JS_ToInt32() (plain numeric coercion), not Postgres's
//                  int4in() text parser -- so a non-numeric string like
//                  "INSERTED: 100" silently becomes 0 (the same result as
//                  plain JS `Number("INSERTED: 100")` -> NaN -> ToInt32 ->
//                  0), not a thrown error. That's PLJS's real, documented
//                  Datum-conversion behavior, not a pljs_fdw bug -- and
//                  the reason this module's own tests use text columns
//                  whenever `returning` is exercised.
module.exports = (function() {

function generateValue(mode, col, index) {
  switch (mode) {
    case 'string':
      return col + ' ' + index;
    case 'date':
      // A real Date object, not an ISO string: PLJS's Datum conversion for
      // date/timestamp/timestamptz columns (pljs_jsvalue_to_datum, types.c)
      // only recognizes an actual JS Date object (checked via Is_Date()) --
      // anything else, including a string, is treated as an unrecognized
      // value for that column and becomes SQL NULL.
      return new Date(Date.UTC(2020, 0, 1 + index));
    case 'encoding':
      // U+00E9 U+00E0 U+00A4, spelled as \u escapes rather than literal
      // non-ASCII source bytes so the exact codepoints are unambiguous
      // regardless of this file's own on-disk encoding.
      return '\u00e9\u00e0\u00a4';
    case 'null':
      return null;
    case 'int':
    default:
      return index;
  }
}

function generateRows(numRows, mode, columns) {
  const rows = [];
  for (let i = 0; i < numRows; i++) {
    const row = {};
    for (const col of columns) {
      row[col] = generateValue(mode, col, i);
    }
    rows.push(row);
  }
  return rows;
}

// A timestamp/timestamptz/date qual's value arrives as a real JS Date
// object (PLJS's pljs_datum_to_jsvalue uses JS_NewDate() for those types,
// symmetric with 'date' mode's own Date-object values above) -- reduced to
// its epoch-ms number here so `=`/`<>` compare by value, not by reference
// (two distinct Date instances for the same instant are never `===`).
function toComparable(v) {
  return v instanceof Date ? v.getTime() : v;
}

function evalQual(row, qual) {
  const v = toComparable(row[qual.column]);
  const t = toComparable(qual.value);
  switch (qual.operator) {
    case '=': return v === t;
    case '<>': return v !== t;
    case '<': return v < t;
    case '<=': return v <= t;
    case '>': return v > t;
    case '>=': return v >= t;
    default: return true; // unrecognized operator: don't filter it out
  }
}

function compareForSort(a, b, sortkeys) {
  for (const sk of sortkeys) {
    const av = a[sk.column];
    const bv = b[sk.column];
    let cmp;

    if (av === null && bv === null) {
      cmp = 0;
    } else if (av === null) {
      cmp = sk.nullsFirst ? -1 : 1;
    } else if (bv === null) {
      cmp = sk.nullsFirst ? 1 : -1;
    } else {
      cmp = av < bv ? -1 : av > bv ? 1 : 0;
    }

    if (sk.direction === 'desc') cmp = -cmp;
    if (cmp !== 0) return cmp;
  }
  return 0;
}

class SelfTestFdw {
  constructor(options) {
    this.numRows = options.numRows ? parseInt(options.numRows, 10) : 20;
    this.mode = options.mode || 'int';
    this._rowIdColumn = options.rowIdColumn || null;
    this.returning = options.returning === 'true';
  }

  get rowIdColumn() {
    return this._rowIdColumn;
  }

  estimate(quals, columns) {
    return this.numRows;
  }

  *execute(quals, columns, sortkeys) {
    let rows = generateRows(this.numRows, this.mode, columns);

    if (quals && quals.length > 0) {
      rows = rows.filter((row) => quals.every((q) => evalQual(row, q)));
    }

    if (sortkeys && sortkeys.length > 0) {
      rows = rows.slice().sort((a, b) => compareForSort(a, b, sortkeys));
    }

    yield* rows;
  }

  insert(row) {
    pljs.elog(INFO, 'self_test_fdw insert: ' + JSON.stringify(row));

    if (this.returning) {
      const out = {};
      for (const k in row) out[k] = 'INSERTED: ' + row[k];
      return out;
    }
  }

  update(rowId, row) {
    pljs.elog(INFO,
              'self_test_fdw update: rowId=' + rowId + ' ' +
                  JSON.stringify(row));

    if (this.returning) {
      const out = {};
      for (const k in row) out[k] = 'UPDATED: ' + row[k];
      return out;
    }
  }

  delete(rowId) {
    pljs.elog(INFO, 'self_test_fdw delete: rowId=' + rowId);
  }

  updateWhere(quals, values) {
    // No prior execute() call is guaranteed before this, so the column
    // list is approximated from quals' own columns plus whatever's being
    // set -- enough to self-verify the match count, which is this
    // function's only real job.
    const cols = quals.map((q) => q.column).concat(Object.keys(values));
    const rows = generateRows(this.numRows, this.mode, cols);
    const count =
        rows.filter((row) => quals.every((q) => evalQual(row, q))).length;

    pljs.elog(INFO, 'self_test_fdw updateWhere: quals=' +
                        JSON.stringify(quals) +
                        ' values=' + JSON.stringify(values) +
                        ' matched=' + count);

    return count;
  }

  deleteWhere(quals) {
    const cols = quals.map((q) => q.column);
    const rows = generateRows(this.numRows, this.mode, cols);
    const count =
        rows.filter((row) => quals.every((q) => evalQual(row, q))).length;

    pljs.elog(INFO, 'self_test_fdw deleteWhere: quals=' +
                        JSON.stringify(quals) + ' matched=' + count);

    return count;
  }

  truncate(options) {
    pljs.elog(INFO, 'self_test_fdw truncate: ' + JSON.stringify(options));
  }
}

return SelfTestFdw;

})();
