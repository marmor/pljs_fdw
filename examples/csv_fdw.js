// csv_fdw: a read-only FDW exposing a local CSV file as a foreign table.
// The pljs_fdw port of Multicorn's csvfdw.py, which wraps Python's stdlib
// csv module -- nothing in the original is outdated (pure stdlib, no
// dependency to speak of), so this is a faithful behavioral port, not a
// modernization.
//
// One deliberate design difference from the original, not just a
// limitation: csvfdw.py maps CSV fields to columns purely by POSITION,
// always against the full declared column list (`line[:len(self.columns)]`),
// regardless of what pljs_fdw calls "column pruning" -- pljs_fdw's own
// execute(quals, columns) only ever receives the *subset* of columns a
// query actually needs, not the full declared list, so positional mapping
// against that pruned/reordered subset would silently misalign values
// under real column pruning (e.g. `SELECT qty FROM t` needing only the
// 4th CSV field would instead read whatever's in field 1). Rather than
// inherit that latent fragility, this module defaults to reading a header
// row and mapping fields to columns BY NAME (immune to pruning/column
// order); pljs_jsvalue_to_datums already ignores any row property that
// isn't one of the target row's actual columns, so extra fields in the
// header are harmless. header=false + an explicit columns option (naming
// every field in on-disk order) is available for headerless files,
// matching the original's positional behavior exactly, deliberately opt-in
// rather than default.
//
// Options:
//   filename  -- full path to the CSV file (required)
//   delimiter -- field delimiter (default ',')
//   quotechar -- quote character (default '"'); a doubled quotechar inside
//                a quoted field is the standard CSV escape for a literal
//                quote character
//   header    -- 'false' to disable header-row/by-name mapping (default:
//                enabled); if disabled, the `columns` option below becomes
//                required
//   columns   -- comma-separated field names in on-disk order, only used
//                (and only required) when header='false'
//   skipRows  -- extra rows to skip before the header/data starts, e.g.
//                for a file with leading comment/title lines (default 0)
//
// UNLIKE the original: csvfdw.py's execute() takes `quals` but never
// filters by them, relying on Postgres to recheck every row locally
// afterward. That's safe under Multicorn's own model, but NOT under
// pljs_fdw's -- confirmed by testing, not just reasoning about it:
// pljs_fdw's qual-pushdown recognizer (pljs_fdw_extract_pushable_quals,
// fdw.c) is a purely syntactic, always-on pattern match over the query's
// WHERE clause with no way for a module to opt out, and ARCHITECTURE.md's
// "trust, not verify" pushdown model means Postgres skips its own local
// recheck for whatever it recognizes and offers. A `csv_fdw` that ignored
// `quals` the way the original does would silently return every row
// regardless of the query's WHERE clause -- reproduced directly against a
// real query while building this port (`WHERE id = '3'` returned all
// rows, unfiltered). So, unlike the header-vs-positional difference
// above (a deliberate improvement), this one is a hard requirement:
// execute() below actually filters by `quals`, matching what every other
// FDW in this project already does.
module.exports = (function() {

/**
 * @brief Parses CSV text into an array of rows (each an array of string
 * fields), by hand rather than with RegExp -- deliberately: this text
 * comes from pljsFdwFs.readFile(), and matching a RegExp against a
 * pljsFdwFs-derived string has been observed to crash the backend from
 * memory corruption when the match result is used further (see
 * docs/ARCHITECTURE.md's "A QuickJS regex/native-string bug" section);
 * plain string indexing avoids it entirely.
 *
 * Handles standard CSV quoting: a quoted field may contain the delimiter
 * or a literal newline; a doubled quotechar inside a quoted field is an
 * escaped literal quotechar. Row separators \n and \r\n are both
 * recognized. Malformed/unterminated quoting at end of file is closed out
 * rather than throwing.
 */
function parseCsv(text, delimiter, quotechar) {
  const rows = [];
  let row = [];
  let field = '';
  let inQuotes = false;
  const n = text.length;
  let i = 0;

  function endField() {
    row.push(field);
    field = '';
  }

  function endRow() {
    endField();
    rows.push(row);
    row = [];
  }

  while (i < n) {
    const c = text[i];

    if (inQuotes) {
      if (c === quotechar) {
        if (text[i + 1] === quotechar) {
          field += quotechar;
          i += 2;
        } else {
          inQuotes = false;
          i += 1;
        }
      } else {
        field += c;
        i += 1;
      }
      continue;
    }

    if (c === quotechar && field.length === 0) {
      inQuotes = true;
      i += 1;
    } else if (c === delimiter) {
      endField();
      i += 1;
    } else if (c === '\r') {
      if (text[i + 1] === '\n') i += 1;
      endRow();
      i += 1;
    } else if (c === '\n') {
      endRow();
      i += 1;
    } else {
      field += c;
      i += 1;
    }
  }

  // Flush a final row left over when the file doesn't end in a newline.
  if (field.length > 0 || row.length > 0) {
    endRow();
  }

  return rows;
}

/**
 * @brief Coerces @p v to a number if it looks like one, else returns it
 * unchanged. Every CSV field value is a string, but a qual's constant
 * arrives already converted to the *local column's* declared type (e.g.
 * a JS number for an int4 column) -- comparing the string "3" against the
 * number 3 with strict equality would never match, so both sides are
 * compared numerically whenever they both parse as numbers, and as
 * strings otherwise.
 */
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
    default: return true; // unrecognized operator: don't filter it out
  }
}

class CsvFdw {
  constructor(options) {
    if (!options.filename) {
      throw new Error('csv_fdw: "filename" option is required');
    }

    this.filename = options.filename;
    this.delimiter = options.delimiter || ',';
    this.quotechar = options.quotechar || '"';
    this.header = options.header !== 'false';
    this.skipRows = options.skipRows ? parseInt(options.skipRows, 10) : 0;
    this.fileColumns =
        options.columns ? options.columns.split(',').map((s) => s.trim())
                        : null;

    if (!this.header && !this.fileColumns) {
      throw new Error(
          'csv_fdw: header=\'false\' requires the "columns" option ' +
          '(comma-separated field names, in on-disk order)');
    }
  }

  *execute(quals, columns) {
    const text = pljsFdwFs.readFile(this.filename);
    const rows = parseCsv(text, this.delimiter, this.quotechar);

    let start = this.skipRows;
    let fieldNames = this.fileColumns;

    if (this.header) {
      fieldNames = rows[start] || [];
      start += 1;
    }

    for (let i = start; i < rows.length; i++) {
      const raw = rows[i];
      const row = {};

      for (let j = 0; j < fieldNames.length && j < raw.length; j++) {
        row[fieldNames[j]] = raw[j];
      }

      if (!quals || quals.every((q) => evalQual(row, q))) {
        yield row;
      }
    }
  }
}

return CsvFdw;

})();
