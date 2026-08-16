// remote_pg_fdw: a generic remote-Postgres FDW built on pgwire.js. This is
// the pljs_fdw equivalent of Multicorn's sqlalchemyfdw, scoped to Postgres
// specifically (see docs/ARCHITECTURE.md and the port discussion this was
// built from -- SQLAlchemy's own database-agnosticism comes from Python's
// mature driver ecosystem, which QuickJS has no equivalent of; this is
// "the same idea, one backend").
//
// Options (server/table level, merged per the usual pljs_fdw rules):
//   host, port, user, password, database -- connection info
//   table   -- remote table name (required)
//   rowIdColumn -- remote column identifying a row for update()/delete()
//                  (optional; omit to only support read + insert + direct
//                  modify via updateWhere/deleteWhere)
// column_name (per-column option, standard pljs_fdw mechanism) maps a local
// column to a differently-named remote one; see docs/FDW_API.md.
//
// Row values come back from pgwire as strings (text wire format) -- left
// as-is here rather than converted in JS. pljs_jsvalue_to_datums (the
// unmodified PLJS conversion this framework reuses) converts a JS string
// into any local column's actual type via that type's normal input
// function, the same as any other text-format value -- exactly like COPY
// or the simple query protocol already work, nothing FDW-specific.
module.exports = (function() {

// pljs.require('pgwire') deliberately isn't called here, at this module's
// own top level: pljs.require() can't be called while another module's
// top-level evaluation is still on the stack (i.e. nested inside another
// pljs_module_require()/pljs.require() load) -- it silently breaks module
// loading (surfaces upstream as fdw.c's generic "not a constructor", not a
// catchable JS exception here). Deferred to the constructor instead, which
// runs later, once this module's own load has fully finished.

function quoteIdent(name) {
  return '"' + String(name).replace(/"/g, '""') + '"';
}

class RemotePgFdw {
  constructor(options) {
    if (!options.table) {
      throw new Error('remote_pg_fdw: "table" option is required');
    }

    const {PgConnection} = pljs.require('pgwire');

    this.conn = new PgConnection({
      host: options.host || '127.0.0.1',
      port: options.port ? parseInt(options.port, 10) : 5432,
      user: options.user,
      password: options.password,
      database: options.database || options.user,
    });

    this.table = options.table;
    this.rowIdColumn = options.rowIdColumn || null;
  }

  _remoteName(localName, columnMap) {
    return (columnMap && columnMap[localName]) || localName;
  }

  _selectList(columns, columnMap) {
    if (!columns || columns.length === 0) {
      return '*';
    }
    return columns
        .map((c) => quoteIdent(this._remoteName(c, columnMap)) + ' AS ' +
                quoteIdent(c))
        .join(', ');
  }

  /** @returns {clause: 'col1 = $1 AND col2 > $2' or '', params: [...]} */
  _whereClause(quals, columnMap, paramOffset) {
    if (!quals || quals.length === 0) {
      return {clause: '', params: []};
    }

    const parts = [];
    const params = [];

    quals.forEach((q, i) => {
      parts.push(quoteIdent(this._remoteName(q.column, columnMap)) + ' ' +
                q.operator + ' $' + (paramOffset + i + 1));
      params.push(q.value);
    });

    return {clause: ' WHERE ' + parts.join(' AND '), params};
  }

  _orderByClause(sortkeys, columnMap) {
    if (!sortkeys || sortkeys.length === 0) {
      return '';
    }

    const parts = sortkeys.map(
        (sk) => quoteIdent(this._remoteName(sk.column, columnMap)) + ' ' +
            (sk.direction === 'desc' ? 'DESC' : 'ASC') + ' ' +
            (sk.nullsFirst ? 'NULLS FIRST' : 'NULLS LAST'));

    return ' ORDER BY ' + parts.join(', ');
  }

  estimate(quals, columns, columnMap) {
    const where = this._whereClause(quals, columnMap, 0);
    const result = this.conn.query(
        'SELECT count(*) AS n FROM ' + quoteIdent(this.table) + where.clause,
        where.params);
    return parseInt(result.rows[0].n, 10);
  }

  *execute(quals, columns, sortkeys, columnMap) {
    const where = this._whereClause(quals, columnMap, 0);
    const sql = 'SELECT ' + this._selectList(columns, columnMap) + ' FROM ' +
        quoteIdent(this.table) + where.clause +
        this._orderByClause(sortkeys, columnMap);

    const result = this.conn.query(sql, where.params);
    yield* result.rows;
  }

  insert(row) {
    // Columns the local INSERT left unspecified come through as {col:
    // null} (present, not absent), same as any column value -- explicitly
    // inserting NULL there would override a remote DEFAULT/serial instead
    // of using it, so those are left out of the statement entirely.
    const cols = Object.keys(row).filter((c) => row[c] !== null &&
                                          row[c] !== undefined);
    const values = cols.map((c) => row[c]);
    const placeholders = cols.map((c, i) => '$' + (i + 1));

    const sql = 'INSERT INTO ' + quoteIdent(this.table) + ' (' +
        cols.map((c) => quoteIdent(c)).join(', ') + ') VALUES (' +
        placeholders.join(', ') + ') RETURNING *';

    const result = this.conn.query(sql, values);
    return result.rows[0];
  }

  insertMany(rows) {
    if (rows.length === 0) return [];

    // Simplification: the column list (and which columns get a remote
    // DEFAULT applied) is decided from the first row only, same reasoning
    // as insert() above. Fine for the common case of a batch uniformly
    // omitting the same auto-generated column throughout; a batch mixing
    // explicit and omitted values for the same column across rows isn't
    // handled specially here.
    const cols = Object.keys(rows[0]).filter((c) => rows[0][c] !== null &&
                                              rows[0][c] !== undefined);
    const values = [];
    const rowPlaceholders = rows.map((row) => {
      const placeholders = cols.map((c) => {
        values.push(row[c]);
        return '$' + values.length;
      });
      return '(' + placeholders.join(', ') + ')';
    });

    const sql = 'INSERT INTO ' + quoteIdent(this.table) + ' (' +
        cols.map((c) => quoteIdent(c)).join(', ') + ') VALUES ' +
        rowPlaceholders.join(', ') + ' RETURNING *';

    const result = this.conn.query(sql, values);
    return result.rows;
  }

  update(rowId, row) {
    const cols = Object.keys(row);
    const values = cols.map((c) => row[c]);
    const setClause =
        cols.map((c, i) => quoteIdent(c) + ' = $' + (i + 1)).join(', ');

    const sql = 'UPDATE ' + quoteIdent(this.table) + ' SET ' + setClause +
        ' WHERE ' + quoteIdent(this.rowIdColumn) + ' = $' +
        (values.length + 1) + ' RETURNING *';

    const result = this.conn.query(sql, values.concat([rowId]));
    return result.rows[0];
  }

  delete(rowId) {
    this.conn.query(
        'DELETE FROM ' + quoteIdent(this.table) + ' WHERE ' +
            quoteIdent(this.rowIdColumn) + ' = $1',
        [rowId]);
  }

  updateWhere(quals, values) {
    const cols = Object.keys(values);
    const setClause =
        cols.map((c, i) => quoteIdent(c) + ' = $' + (i + 1)).join(', ');
    const where = this._whereClause(quals, null, cols.length);

    const sql = 'UPDATE ' + quoteIdent(this.table) + ' SET ' + setClause +
        where.clause;

    const result = this.conn.query(
        sql, cols.map((c) => values[c]).concat(where.params));
    return result.rowCount;
  }

  deleteWhere(quals) {
    const where = this._whereClause(quals, null, 0);
    const result = this.conn.query(
        'DELETE FROM ' + quoteIdent(this.table) + where.clause, where.params);
    return result.rowCount;
  }

  truncate(options) {
    this.conn.query(
        'TRUNCATE ' + quoteIdent(this.table) +
        (options.restartSequences ? ' RESTART IDENTITY' : '') +
        (options.cascade ? ' CASCADE' : ''));
  }
}

return RemotePgFdw;

})();
