#include "postgres.h"

#include "access/cmptype.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_foreign_data_wrapper_d.h"
#include "catalog/pg_foreign_server_d.h"
#include "catalog/pg_foreign_table_d.h"
#include "catalog/pg_user_mapping_d.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "fmgr.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "optimizer/appendinfo.h"
#include "parser/parsetree.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "storage/itemptr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif

// Deliberately not including PLJS's own pljs.h: the only thing this file
// would use from it is pljs_type, always passed as NULL to
// pljs_jsvalue_to_datums() and never dereferenced here, so it's forward-
// declared as an opaque type instead. That plus resolving the handful of
// PLJS functions this file calls at runtime (see "PLJS function binding"
// below) rather than linking against pljs.so means this file has zero
// build-time dependency on PLJS's source tree -- only a runtime one, on a
// compatible pljs.so being installed.
#include "access/heapam.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "utils/palloc.h"
#include "windowapi.h"

#include "deps/quickjs/quickjs-libc.h"
#include "deps/quickjs/quickjs.h"

typedef struct pljs_type pljs_type;

PG_MODULE_MAGIC;

/*
 * This file is the PLJS-based Foreign Data Wrapper handler: the FdwRoutine
 * callback table PostgreSQL calls during planning and execution of a
 * foreign table backed by pljs_fdw. It is not reachable from SQL until
 * `CREATE FUNCTION ... RETURNS fdw_handler` / `CREATE FOREIGN DATA WRAPPER
 * ... HANDLER` register it (see pljs_fdw.sql).
 *
 * This is its own PGXS extension (pljs_fdw, this directory's Makefile/
 * control/SQL script), separate from pljs itself, with `requires = 'pljs'`
 * in pljs_fdw.control: `CREATE EXTENSION pljs_fdw` refuses to run unless
 * `CREATE EXTENSION pljs` already has. pljs_fdw.so is NOT linked against
 * pljs.so at build time; it resolves the handful of PLJS functions it needs
 * at runtime instead (see "PLJS function binding" below) -- those are the
 * only symbols pljs.so exports with PGDLLEXPORT (see pljs's src/pljs.h) for
 * exactly this purpose.
 *
 * A foreign table's implementation is a CommonJS module (loaded via
 * pljs_module_require(), i.e. the same `pljs.modules` table `pljs.require()`
 * reads from) named by a `module` option on the foreign table or its
 * server, whose `module.exports` is a class:
 *
 *   module.exports = class {
 *     constructor(options) { ... }               // merged server+table options
 *     estimate(quals, columns, columnMap) { return N; }     // optional; row estimate
 *     *execute(quals, columns, sortkeys, columnMap) { ... } // generator, yields row objects
 *     insert(row) { ... }                         // optional; enables INSERT + COPY FROM
 *     insertMany(rows) { ... }                    // optional; enables batched INSERT/COPY
 *     get rowIdColumn() { return 'id'; }          // required for update()/delete()
 *     update(rowId, row) { ... }                  // optional; enables per-row UPDATE
 *     delete(rowId) { ... }                       // optional; enables per-row DELETE
 *     updateWhere(quals, values) { return N; }    // optional; enables direct UPDATE
 *     deleteWhere(quals) { return N; }            // optional; enables direct DELETE
 *     truncate({ cascade, restartSequences }) { ... } // optional; enables TRUNCATE
 *   }
 *
 * The required read-only callbacks (GetForeignRelSize..EndForeignScan),
 * ExplainForeignScan, the write callbacks (IsForeignRelUpdatable,
 * AddForeignUpdateTargets, BeginForeignModify, ExecForeignInsert/Update/
 * Delete, BeginForeignInsert for `COPY foreign_table FROM ...`,
 * GetForeignModifyBatchSize/ExecForeignBatchInsert, ExplainForeignModify,
 * PlanDirectModify/BeginDirectModify/IterateDirectModify/
 * ExplainDirectModify), ExecForeignTruncate, and ImportForeignSchema are
 * wired up. `quals` (simple `column <op> constant` restrictions) and
 * `sortkeys` (a leading prefix of the query's ORDER BY matching plain
 * columns) are both pushed down for real: what's recognized is trusted to
 * have been honored by execute() and is not verified locally (no recheck
 * qual, no Sort node) -- see the "Qual pushdown" and "Sort pushdown"
 * sections below. `columns` reflects real pruning too (see "Column
 * pruning"). `columnMap` reflects real column-name mapping (see "Column
 * mapping"). Write support is per-operation opt-in (see the "Write
 * support", "Direct modify", and "TRUNCATE" sections). See "IMPORT
 * FOREIGN SCHEMA" for the (differently-shaped) schema-describing module
 * contract. Everything else optional in FdwRoutine not mentioned above is
 * still deferred.
 *
 * Deliberately NOT planned: DSM-based parallel scan
 * (IsForeignScanParallelSafe/EstimateDSMForeignScan/etc.). Even
 * postgres_fdw -- the reference FDW -- implements none of these. Without
 * an FDW building its own worker-coordination via those DSM callbacks
 * (core Postgres only does this automatically for its own scan types,
 * e.g. a shared block-range cursor for SeqScan), marking a foreign scan
 * parallel-safe just means every worker in a Gather independently re-runs
 * the whole scan and the results get unioned -- i.e. every row silently
 * duplicated once per worker. Doing this correctly would mean redesigning
 * execute() to accept a worker index/count so the JS side partitions its
 * own rows, not just wiring up a few callbacks.
 *
 * Also deliberately NOT planned, for a different reason: async execution
 * (IsForeignPathAsyncCapable/ForeignAsyncRequest/ForeignAsyncConfigureWait/
 * ForeignAsyncNotify). Unlike parallel scan, postgres_fdw genuinely uses
 * this -- but its whole point is overlapping wait time on a pollable
 * resource: ForeignAsyncConfigureWait registers the underlying libpq
 * connection's socket file descriptor on the parent Append node's
 * WaitEventSet, so while one remote connection is waiting on a round trip,
 * Postgres can poll a different foreign scan's connection instead of
 * blocking on each sequentially. pljs_fdw's execute() is a synchronous
 * QuickJS generator call with no pollable resource underneath it at all --
 * pljs_fdw_extend_namespace() is still a no-op placeholder, with no
 * fetch() or non-blocking socket exposed to JS. Wiring up the async
 * callbacks without a real async I/O primitive underneath would be pure
 * ceremony: there'd be nothing to overlap, so zero wall-clock benefit for
 * a UNION ALL/partition scan across several pljs_fdw tables, just added
 * state-machine complexity. Revisit only if/when a genuine async I/O
 * primitive (e.g. a non-blocking fetch() bridged to Postgres's wait-event
 * mechanism) gets built into the FDW namespace -- a substantial feature
 * in its own right, separate from the async callbacks themselves.
 *
 * Deliberately self-contained: nothing in PLJS's own files (pljs.c, cache.c,
 * functions.c, modules.c, pljs.h) is touched to support the FDW, beyond the
 * PGDLLEXPORT markers on the handful of functions this file calls and the
 * (independently-motivated) NULL-fcinfo fix in types.c -- those two are the
 * only diff against upstream PLJS, and the only things that would need
 * proposing there. The JS namespace setup calls the ordinary, unmodified
 * pljs_setup_namespace() and layers FDW-only bindings on top locally (see
 * pljs_fdw_extend_namespace); the FDW's own JSContext/instance cache below
 * has its own MemoryContext, its own JSRuntime (pljs_fdw_rt, independent of
 * PLJS's global `rt`), and is self-initializing via a constructor attribute
 * rather than hooking into _PG_init or the shared cache lifecycle. Living in
 * its own extension (see above) rather than being folded into pljs.so is
 * this same principle taken one step further: this directory can be dropped
 * in, pulled out, or released on its own schedule without touching pljs at
 * all beyond those two small, independently-justifiable changes.
 *
 * The JSContext + instantiated class for a given (role, foreign table) are
 * cached across scans, so BeginForeignScan only pays the module-load/
 * construct cost once per backend per table. Only the per-scan iterator
 * returned by execute() is scan-owned and freed in EndForeignScan; the
 * cached ctx/instance persist for the life of the backend.
 *
 * The cache is invalidated two ways: `ALTER FOREIGN TABLE/SERVER/USER
 * MAPPING ... OPTIONS` (option changes, e.g. to "module" or per-user
 * credentials) flushes the whole cache via a syscache callback
 * (pljs_fdw_check_invalidation, checked at the top of BeginForeignScan); a
 * source change to the specific cached module row (`UPDATE pljs.modules
 * SET source = ...`) is detected per-entry via an xmin/ctid comparison
 * (pljs_fdw_module_row_version) and evicts just that entry.
 */

/*
 * ---------- PLJS function binding ----------
 *
 * The handful of PLJS functions this file calls (pljs_setup_namespace,
 * pljs_module_require, pljs_datum_to_jsvalue, pljs_jsvalue_to_datums,
 * pljs_js_array_length, pljs_dump_error -- all PGDLLEXPORT in
 * ../src/pljs.h) are resolved at runtime via load_external_function()/
 * lookup_external_function() (fmgr.h) rather than linked directly against
 * pljs.so at build time. Direct linking (a DT_NEEDED on pljs.so) was tried
 * first and reverted: Postgres's dfmgr does dlsym(handle, "_PG_init") on
 * every library it dlopen()s to find that library's own init function, but
 * glibc's dlsym on a dlopen() handle also searches that handle's DT_NEEDED
 * dependencies -- so it found pljs.so's _PG_init through the dependency
 * edge and called it a second time in the same backend, hitting "attempt to
 * redefine parameter pljs.memory_limit". load_external_function() is the
 * same official path Postgres itself uses to load a library (with its own
 * de-duplication, keyed by filename, tracked in dfmgr's own cache), so
 * routing through it here means pljs's _PG_init only ever runs once no
 * matter which backend/order first touches pljs.so.
 */

typedef void (*pljs_setup_namespace_fn)(JSContext *ctx);
typedef JSValue (*pljs_module_require_fn)(JSContext *ctx,
                                          const char *module_name);
typedef JSValue (*pljs_datum_to_jsvalue_fn)(Oid argtype, Datum arg,
                                            bool is_null,
                                            bool expand_composite,
                                            JSContext *ctx);
typedef Datum *(*pljs_jsvalue_to_datums_fn)(pljs_type *type, JSValue val,
                                            bool **is_null, TupleDesc tupdesc,
                                            JSContext *ctx);
typedef uint32_t (*pljs_js_array_length_fn)(JSValue val, JSContext *ctx);
typedef char *(*pljs_dump_error_fn)(JSContext *ctx);

static pljs_setup_namespace_fn p_pljs_setup_namespace;
static pljs_module_require_fn p_pljs_module_require;
static pljs_datum_to_jsvalue_fn p_pljs_datum_to_jsvalue;
static pljs_jsvalue_to_datums_fn p_pljs_jsvalue_to_datums;
static pljs_js_array_length_fn p_pljs_js_array_length;
static pljs_dump_error_fn p_pljs_dump_error;

// Call-site-transparent: every existing call to these six names elsewhere
// in this file goes through the resolved pointer instead, no other line
// in this file needs to change.
#define pljs_setup_namespace p_pljs_setup_namespace
#define pljs_module_require p_pljs_module_require
#define pljs_datum_to_jsvalue p_pljs_datum_to_jsvalue
#define pljs_jsvalue_to_datums p_pljs_jsvalue_to_datums
#define pljs_js_array_length p_pljs_js_array_length
#define pljs_dump_error p_pljs_dump_error

/**
 * @brief Resolves the PLJS functions above, the first time any of them is
 * actually needed. Not done in pljs_fdw_module_init() (the constructor
 * below): that runs while Postgres's own dlopen() of this library is still
 * on the stack, and calling back into Postgres's library-loading machinery
 * (load_external_function -> internal_load_library -> dlopen) from there is
 * best avoided. Called instead from pljs_fdw_handler(), the single entry
 * point every FDW operation passes through first.
 */
static void pljs_fdw_bind_pljs_functions(void) {
  if (p_pljs_setup_namespace != NULL) {
    return;
  }

  void *filehandle;
  p_pljs_setup_namespace = (pljs_setup_namespace_fn)load_external_function(
      "$libdir/pljs", "pljs_setup_namespace", true, &filehandle);
  p_pljs_module_require = (pljs_module_require_fn)lookup_external_function(
      filehandle, "pljs_module_require");
  p_pljs_datum_to_jsvalue =
      (pljs_datum_to_jsvalue_fn)lookup_external_function(
          filehandle, "pljs_datum_to_jsvalue");
  p_pljs_jsvalue_to_datums =
      (pljs_jsvalue_to_datums_fn)lookup_external_function(
          filehandle, "pljs_jsvalue_to_datums");
  p_pljs_js_array_length = (pljs_js_array_length_fn)lookup_external_function(
      filehandle, "pljs_js_array_length");
  p_pljs_dump_error =
      (pljs_dump_error_fn)lookup_external_function(filehandle,
                                                    "pljs_dump_error");
}

/*
 * ---------- JS namespace ----------
 */

/**
 * @brief Adds FDW-only bindings on top of the ordinary trusted namespace.
 *
 * Called after the unmodified pljs_setup_namespace(), which is all any
 * trusted `LANGUAGE pljs` context ever gets. This is the place for
 * filesystem/network bindings an FDW implementation needs -- safe here
 * because this context is only ever reachable through BeginForeignScan,
 * which in turn is only reachable via CREATE FOREIGN DATA WRAPPER/SERVER,
 * gated by Postgres's own (superuser-only) FDW privilege model rather than
 * the PL trusted/untrusted mechanism. Currently a no-op placeholder.
 *
 * @param ctx #JSContext Javascript context, already set up via
 *            pljs_setup_namespace().
 */
static void pljs_fdw_extend_namespace(JSContext *ctx) {
}

/*
 * ---------- FDW context cache ----------
 *
 * Self-contained: its own MemoryContext (not cache.c's), its own hash
 * table, its own init (via __attribute__((constructor)) below rather than
 * a call from _PG_init).
 */

typedef struct pljs_fdw_context_cache_key {
  Oid user_id;
  Oid foreigntableid;
} pljs_fdw_context_cache_key;

typedef struct pljs_fdw_context_cache_value {
  pljs_fdw_context_cache_key key;
  JSContext *ctx;
  JSValue instance;

  // Row version of the module in pljs.modules this entry was built from,
  // used to detect a source change (e.g. `UPDATE pljs.modules SET
  // source = ...`) without re-reading/re-parsing the source on every scan.
  char *module_path;
  TransactionId module_xmin;
  ItemPointerData module_tid;
} pljs_fdw_context_cache_value;

static MemoryContext pljs_fdw_cache_memory_context = NULL;
static HTAB *pljs_fdw_context_HashTable = NULL;

// This file's own JSRuntime, independent of PLJS's global `rt` -- created
// once in pljs_fdw_module_init(). Cheap (JS_NewRuntime() is a single call)
// and removes the one place this file previously read PLJS global state
// directly, rather than calling into a stable, reusable PLJS function.
static JSRuntime *pljs_fdw_rt = NULL;

/**
 * @brief Set by #pljs_fdw_syscache_callback, checked by
 * #pljs_fdw_check_invalidation.
 *
 * A bare flag rather than doing the flush directly in the callback: syscache
 * invalidation callbacks can fire at sensitive points, so the convention
 * (mirroring e.g. postgres_fdw's connection cache) is to defer the actual
 * cleanup to a safe point -- here, the top of BeginForeignScan.
 */
static bool pljs_fdw_cache_invalidation_pending = false;

static void pljs_fdw_context_hashtable_create(void) {
  HASHCTL ctl = {0};

  ctl.keysize = sizeof(pljs_fdw_context_cache_key);
  ctl.entrysize = sizeof(pljs_fdw_context_cache_value);
  ctl.hcxt = pljs_fdw_cache_memory_context;

  pljs_fdw_context_HashTable = hash_create(
      "PLJS FDW Context Cache",
      32, // Arbitrary guess at number of (role, foreign table) pairs.
      &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void pljs_fdw_cleanup_context(pljs_fdw_context_cache_value *entry) {
  JS_FreeValue(entry->ctx, entry->instance);
  JS_FreeContext(entry->ctx);
}

/**
 * @brief Cache a JSContext and instantiated FDW class for a scan.
 *
 * Each (user_id, foreigntableid) pair may have at most one cached entry.
 *
 * @param ctx             The #JSContext to cache. Ownership is transferred
 *                        to the cache; the caller must not free it directly.
 * @param instance        The instantiated FDW class #JSValue to cache.
 *                        Ownership is transferred to the cache.
 * @param module_path     The pljs.modules path this entry was built from,
 *                        deep-copied into the cache #MemoryContext.
 * @param module_xmin     The xmin of the module row at load time, used to
 *                        detect a later source change.
 * @param module_tid      The ctid of the module row at load time.
 */
static void pljs_fdw_cache_add(Oid user_id, Oid foreigntableid,
                               JSContext *ctx, JSValue instance,
                               const char *module_path,
                               TransactionId module_xmin,
                               ItemPointerData module_tid) {
  bool found;
  pljs_fdw_context_cache_key key = {.user_id = user_id,
                                    .foreigntableid = foreigntableid};

  pljs_fdw_context_cache_value *hvalue =
      (pljs_fdw_context_cache_value *)hash_search(
          pljs_fdw_context_HashTable, &key, HASH_ENTER, &found);

  if (found) {
    ereport(ERROR, errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("an FDW context cache entry already exists for user_id "
                   "%d, foreigntableid %d",
                   user_id, foreigntableid));
  }

  MemoryContext old_context =
      MemoryContextSwitchTo(pljs_fdw_cache_memory_context);

  hvalue->key = key;
  hvalue->ctx = ctx;
  hvalue->instance = instance;
  hvalue->module_path = pstrdup(module_path);
  hvalue->module_xmin = module_xmin;
  hvalue->module_tid = module_tid;

  MemoryContextSwitchTo(old_context);
}

static pljs_fdw_context_cache_value *pljs_fdw_cache_find(Oid user_id,
                                                          Oid foreigntableid) {
  pljs_fdw_context_cache_key key = {.user_id = user_id,
                                    .foreigntableid = foreigntableid};

  return (pljs_fdw_context_cache_value *)hash_search(
      pljs_fdw_context_HashTable, &key, HASH_FIND, NULL);
}

static void pljs_fdw_cache_remove(Oid user_id, Oid foreigntableid) {
  pljs_fdw_context_cache_key key = {.user_id = user_id,
                                    .foreigntableid = foreigntableid};

  pljs_fdw_context_cache_value *hvalue =
      (pljs_fdw_context_cache_value *)hash_search(
          pljs_fdw_context_HashTable, &key, HASH_REMOVE, NULL);

  if (hvalue) {
    pljs_fdw_cleanup_context(hvalue);
  }
}

/**
 * @brief Syscache invalidation callback for foreign table/server DDL.
 *
 * Registered on the FOREIGNTABLEREL and FOREIGNSERVEROID syscaches, so
 * `ALTER FOREIGN TABLE ... OPTIONS` and `ALTER SERVER ... OPTIONS` (where a
 * pljs_fdw table's "module" option lives) are noticed.
 */
static void pljs_fdw_syscache_callback(Datum arg, int cacheid,
                                       uint32 hashvalue) {
  pljs_fdw_cache_invalidation_pending = true;
}

/**
 * @brief Flushes the whole FDW context cache if a DDL invalidation is
 * pending.
 *
 * Called at the top of BeginForeignScan -- a safe point to actually tear
 * down JSContexts, unlike inside the syscache callback itself. A full flush
 * (rather than identifying the specific changed table/server) mirrors how
 * postgres_fdw invalidates its connection cache: this kind of DDL is rare
 * enough that precision isn't worth the extra bookkeeping.
 */
static void pljs_fdw_check_invalidation(void) {
  if (!pljs_fdw_cache_invalidation_pending) {
    return;
  }

  HASH_SEQ_STATUS status;
  pljs_fdw_context_cache_value *entry;

  hash_seq_init(&status, pljs_fdw_context_HashTable);
  while ((entry = hash_seq_search(&status)) != NULL) {
    pljs_fdw_cleanup_context(entry);
  }

  hash_destroy(pljs_fdw_context_HashTable);
  pljs_fdw_context_hashtable_create();

  pljs_fdw_cache_invalidation_pending = false;
}

/**
 * @brief Self-initializes the FDW cache and JSRuntime when this shared
 * library is loaded, independent of PLJS's own _PG_init.
 *
 * Safe to do this setup this early (before _PG_init runs): it only needs
 * TopMemoryContext, which Postgres has already set up by the time any
 * extension .so is dlopen()'d, and neither creating a JSRuntime nor
 * registering syscache callbacks depends on anything else PLJS
 * initializes later.
 */
__attribute__((constructor)) static void pljs_fdw_module_init(void) {
  pljs_fdw_rt = JS_NewRuntime();

  pljs_fdw_cache_memory_context = AllocSetContextCreate(
      TopMemoryContext, "PLJS FDW Context Cache", ALLOCSET_SMALL_SIZES);

  pljs_fdw_context_hashtable_create();

  CacheRegisterSyscacheCallback(FOREIGNTABLEREL, pljs_fdw_syscache_callback,
                                (Datum)0);
  CacheRegisterSyscacheCallback(FOREIGNSERVEROID, pljs_fdw_syscache_callback,
                                (Datum)0);
  CacheRegisterSyscacheCallback(USERMAPPINGUSERSERVER,
                                pljs_fdw_syscache_callback, (Datum)0);
}

/*
 * ---------- pljs.modules row-version lookup ----------
 *
 * Small, self-contained duplicate of the relid/index lookup modules.c does
 * privately for pljs_read_module(), since importing that logic would mean
 * exporting new surface from modules.c. Column 1 of pljs.modules is `path`
 * (see pljs.sql) -- that's the only part of the schema this depends on.
 */

static bool pljs_fdw_module_row_version(const char *module_name,
                                        TransactionId *xmin,
                                        ItemPointerData *tid) {
  bool found = false;
  Oid schema_oid = get_namespace_oid("pljs", false);
  Oid table_relid = get_relname_relid("modules", schema_oid);
  Oid index_relid = get_relname_relid("pljs_modules_path", schema_oid);

  ScanKeyData scankey[1];
  ScanKeyInit(&scankey[0], 1 /* path */, BTEqualStrategyNumber, F_TEXTEQ,
              CStringGetTextDatum(module_name));

  Relation table = table_open(table_relid, AccessShareLock);
  Relation index = index_open(index_relid, AccessShareLock);

  SysScanDesc scan_descriptor =
      systable_beginscan_ordered(table, index, GetActiveSnapshot(), 1, scankey);

  HeapTuple tuple =
      systable_getnext_ordered(scan_descriptor, ForwardScanDirection);

  if (HeapTupleIsValid(tuple)) {
    *xmin = HeapTupleHeaderGetXmin(tuple->t_data);
    *tid = tuple->t_self;
    found = true;
  }

  systable_endscan_ordered(scan_descriptor);
  index_close(index, AccessShareLock);
  table_close(table, AccessShareLock);

  return found;
}

/*
 * ---------- Get-or-create the cached JSContext/instance ----------
 */

static char *pljs_fdw_get_option(List *options, const char *name);
static JSValue pljs_fdw_build_options(JSContext *ctx, List *server_options,
                                      List *user_mapping_options,
                                      List *table_options);
static bool pljs_fdw_has_user_mapping(Oid userid, Oid serverid);

/**
 * @brief Get the cached JSContext/instance for (current user, foreign
 * table), or build and cache one if none exists yet (or the cached one's
 * module source has since changed).
 *
 * Shared by GetForeignRelSize (planning time, for row estimation) and
 * BeginForeignScan (execution time), so a table's class is instantiated
 * once per backend regardless of how many times it's used across planning
 * and execution.
 */
static void pljs_fdw_get_or_create_instance(Oid foreigntableid,
                                            JSContext **ctx_out,
                                            JSValue *instance_out) {
  pljs_fdw_check_invalidation();

  Oid user_id = GetUserId();

  pljs_fdw_context_cache_value *cached =
      pljs_fdw_cache_find(user_id, foreigntableid);

  if (cached != NULL) {
    // Detect a source change (e.g. `UPDATE pljs.modules SET source = ...`)
    // since this entry was built, and evict it if so.
    TransactionId current_xmin;
    ItemPointerData current_tid;
    bool row_found = pljs_fdw_module_row_version(cached->module_path,
                                                 &current_xmin, &current_tid);

    if (!row_found || current_xmin != cached->module_xmin ||
        !ItemPointerEquals(&current_tid, &cached->module_tid)) {
      pljs_fdw_cache_remove(user_id, foreigntableid);
      cached = NULL;
    }
  }

  if (cached != NULL) {
    *ctx_out = cached->ctx;
    *instance_out = cached->instance;
    return;
  }

  ForeignTable *table = GetForeignTable(foreigntableid);
  ForeignServer *server = GetForeignServer(table->serverid);

  char *module_path = pljs_fdw_get_option(table->options, "module");
  if (module_path == NULL) {
    module_path = pljs_fdw_get_option(server->options, "module");
  }

  if (module_path == NULL) {
    ereport(ERROR, (errmsg("pljs_fdw: foreign table or server must specify "
                           "a \"module\" option")));
  }

  JSContext *ctx = JS_NewContext(pljs_fdw_rt);
  pljs_setup_namespace(ctx);
  pljs_fdw_extend_namespace(ctx);

  JSValue klass = pljs_module_require(ctx, module_path);

  if (JS_IsException(klass)) {
    char *error_message = pljs_dump_error(ctx);
    JS_FreeContext(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: unable to load module \"%s\"",
                           module_path),
                    errdetail("%s", error_message)));
  }

  TransactionId module_xmin = InvalidTransactionId;
  ItemPointerData module_tid = {0};
  pljs_fdw_module_row_version(module_path, &module_xmin, &module_tid);

  List *user_mapping_options = NIL;

  if (pljs_fdw_has_user_mapping(user_id, table->serverid)) {
    UserMapping *um = GetUserMapping(user_id, table->serverid);
    user_mapping_options = um->options;
  }

  JSValue options = pljs_fdw_build_options(ctx, server->options,
                                           user_mapping_options,
                                           table->options);
  JSValueConst ctor_args[] = {options};
  JSValue instance = JS_CallConstructor(ctx, klass, 1, ctor_args);

  JS_FreeValue(ctx, klass);
  JS_FreeValue(ctx, options);

  if (JS_IsException(instance)) {
    char *error_message = pljs_dump_error(ctx);
    JS_FreeContext(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: unable to instantiate FDW class from "
                           "\"%s\"",
                           module_path),
                    errdetail("%s", error_message)));
  }

  // Cache before the first execute()/estimate() call: a transient failure
  // there shouldn't invalidate an otherwise-valid cached class instance.
  pljs_fdw_cache_add(user_id, foreigntableid, ctx, instance, module_path,
                     module_xmin, module_tid);

  *ctx_out = ctx;
  *instance_out = instance;
}

/*
 * ---------- FDW option helpers ----------
 */

static char *pljs_fdw_get_option(List *options, const char *name) {
  ListCell *lc;

  foreach (lc, options) {
    DefElem *def = lfirst_node(DefElem, lc);

    if (strcmp(def->defname, name) == 0) {
      return defGetString(def);
    }
  }

  return NULL;
}

/**
 * @brief Merges server, user mapping, and table options into a single JS
 * object passed to the FDW class's constructor.
 *
 * Later arguments win on a name collision: user_mapping_options (typically
 * per-user credentials -- an API key, token, etc.) override server_options
 * (typically shared connection info), and table_options (the most specific
 * declaration) override both.
 */
static JSValue pljs_fdw_build_options(JSContext *ctx, List *server_options,
                                      List *user_mapping_options,
                                      List *table_options) {
  JSValue obj = JS_NewObject(ctx);
  ListCell *lc;

  foreach (lc, server_options) {
    DefElem *def = lfirst_node(DefElem, lc);
    JS_SetPropertyStr(ctx, obj, def->defname,
                      JS_NewString(ctx, defGetString(def)));
  }

  foreach (lc, user_mapping_options) {
    DefElem *def = lfirst_node(DefElem, lc);
    JS_SetPropertyStr(ctx, obj, def->defname,
                      JS_NewString(ctx, defGetString(def)));
  }

  foreach (lc, table_options) {
    DefElem *def = lfirst_node(DefElem, lc);
    JS_SetPropertyStr(ctx, obj, def->defname,
                      JS_NewString(ctx, defGetString(def)));
  }

  return obj;
}

/**
 * @brief Checks whether the given role has a user mapping for a server,
 * without GetUserMapping()'s behavior of raising an ERROR when none
 * exists -- most pljs_fdw servers won't have one (no per-user credentials
 * needed), which must remain a normal, unexceptional case.
 */
static bool pljs_fdw_has_user_mapping(Oid userid, Oid serverid) {
  if (SearchSysCacheExists2(USERMAPPINGUSERSERVER, ObjectIdGetDatum(userid),
                            ObjectIdGetDatum(serverid))) {
    return true;
  }

  // No mapping for this specific role -- Postgres also falls back to a
  // PUBLIC mapping (userid InvalidOid), same as GetUserMapping() does.
  return SearchSysCacheExists2(USERMAPPINGUSERSERVER,
                               ObjectIdGetDatum(InvalidOid),
                               ObjectIdGetDatum(serverid));
}

/*
 * ---------- Qual pushdown ----------
 *
 * Recognizes simple `column <op> constant` restriction clauses (and their
 * commuted form, `constant <op> column`) from a base relation's
 * baserestrictinfo, for a small allowlisted set of operators. Anything more
 * complex (expressions, non-constant RHS, unsupported operators) is left
 * alone -- Postgres still filters those locally, exactly as if pushdown
 * didn't exist. Recognized quals ARE removed from the local recheck list
 * (see pljs_fdw_get_foreign_plan), so a JS execute() that pushdown is
 * offered to must actually apply it -- like any real FDW pushdown, this is
 * a correctness contract, not just a hint.
 */

typedef struct pljs_fdw_qual {
  AttrNumber attnum;
  char *opname;
  Const *value;
  RestrictInfo *rinfo; // only valid during planning; NULL after decoding
                       // back from a plan's fdw_private at execution time
} pljs_fdw_qual;

static const char *const pljs_fdw_supported_operators[] = {"=",  "<>", "<",
                                                            "<=", ">",  ">="};

static bool pljs_fdw_is_supported_operator(const char *opname) {
  for (size_t i = 0; i < lengthof(pljs_fdw_supported_operators); i++) {
    if (strcmp(opname, pljs_fdw_supported_operators[i]) == 0) {
      return true;
    }
  }

  return false;
}

/**
 * @brief Recognizes a `Var <op> Const` (or commuted) restriction clause.
 *
 * @returns true if @p rinfo is such a clause referencing @p relid directly,
 * with @p opname_out among #pljs_fdw_supported_operators, in which case
 * @p attnum_out/@p opname_out/@p const_out are filled in.
 */
static bool pljs_fdw_get_simple_qual(RestrictInfo *rinfo, Index relid,
                                     AttrNumber *attnum_out, char **opname_out,
                                     Const **const_out) {
  if (!IsA(rinfo->clause, OpExpr)) {
    return false;
  }

  OpExpr *op = (OpExpr *)rinfo->clause;

  if (list_length(op->args) != 2) {
    return false;
  }

  Node *left = strip_implicit_coercions((Node *)linitial(op->args));
  Node *right = strip_implicit_coercions((Node *)lsecond(op->args));

  Var *var;
  Const *cst;
  bool var_on_left;

  if (IsA(left, Var) && IsA(right, Const)) {
    var = (Var *)left;
    cst = (Const *)right;
    var_on_left = true;
  } else if (IsA(right, Var) && IsA(left, Const)) {
    var = (Var *)right;
    cst = (Const *)left;
    var_on_left = false;
  } else {
    return false;
  }

  if ((Index)var->varno != relid || var->varlevelsup != 0 ||
      var->varattno < 1) {
    return false;
  }

  if (cst->constisnull) {
    return false;
  }

  Oid opno = op->opno;

  if (!var_on_left) {
    opno = get_commutator(opno);
    if (!OidIsValid(opno)) {
      return false;
    }
  }

  char *opname = get_opname(opno);

  if (opname == NULL || !pljs_fdw_is_supported_operator(opname)) {
    return false;
  }

  *attnum_out = var->varattno;
  *opname_out = opname;
  *const_out = cst;

  return true;
}

/**
 * @brief Extracts the subset of @p baserestrictinfo that
 * #pljs_fdw_get_simple_qual recognizes.
 */
static List *pljs_fdw_extract_pushable_quals(List *baserestrictinfo,
                                             Index relid) {
  List *quals = NIL;
  ListCell *lc;

  foreach (lc, baserestrictinfo) {
    RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
    AttrNumber attnum;
    char *opname;
    Const *cst;

    if (!pljs_fdw_get_simple_qual(rinfo, relid, &attnum, &opname, &cst)) {
      continue;
    }

    pljs_fdw_qual *qual = (pljs_fdw_qual *)palloc(sizeof(pljs_fdw_qual));

    qual->attnum = attnum;
    qual->opname = opname;
    qual->value = cst;
    qual->rinfo = rinfo;

    quals = lappend(quals, qual);
  }

  return quals;
}

/**
 * @brief Converts a #pljs_fdw_qual list to the `quals` array passed to a JS
 * execute()/estimate() call: `[{column, operator, value}, ...]`.
 */
static JSValue pljs_fdw_quals_to_jsvalue(JSContext *ctx, List *quals,
                                         TupleDesc tupdesc) {
  JSValue arr = JS_NewArray(ctx);
  uint32_t idx = 0;
  ListCell *lc;

  foreach (lc, quals) {
    pljs_fdw_qual *qual = (pljs_fdw_qual *)lfirst(lc);
    Form_pg_attribute attr = TupleDescAttr(tupdesc, qual->attnum - 1);

    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, obj, "column",
                      JS_NewString(ctx, NameStr(attr->attname)));
    JS_SetPropertyStr(ctx, obj, "operator", JS_NewString(ctx, qual->opname));
    JS_SetPropertyStr(
        ctx, obj, "value",
        pljs_datum_to_jsvalue(qual->value->consttype, qual->value->constvalue,
                              qual->value->constisnull, false, ctx));

    JS_SetPropertyUint32(ctx, arr, idx++, obj);
  }

  return arr;
}

/**
 * @brief Converts a #pljs_fdw_qual list into a plan-safe representation for
 * ForeignScan.fdw_private: a List of (Integer attnum, String opname, Const
 * value) triples. Plain pljs_fdw_qual structs can't go in fdw_private
 * directly -- it must be built from proper Node types, since plan trees can
 * be copied/serialized (e.g. for prepared-statement plan caching).
 */
static List *pljs_fdw_quals_to_plan_private(List *quals) {
  List *result = NIL;
  ListCell *lc;

  foreach (lc, quals) {
    pljs_fdw_qual *qual = (pljs_fdw_qual *)lfirst(lc);
    List *triple = list_make3(makeInteger(qual->attnum),
                              makeString(qual->opname), (Node *)qual->value);

    result = lappend(result, triple);
  }

  return result;
}

/**
 * @brief Inverse of #pljs_fdw_quals_to_plan_private, used at execution
 * time. The resulting quals' `rinfo` field is NULL (only meaningful during
 * planning).
 */
static List *pljs_fdw_quals_from_plan_private(List *fdw_private) {
  List *result = NIL;
  ListCell *lc;

  foreach (lc, fdw_private) {
    List *triple = lfirst_node(List, lc);
    pljs_fdw_qual *qual = (pljs_fdw_qual *)palloc(sizeof(pljs_fdw_qual));

    qual->attnum = (AttrNumber)intVal(linitial(triple));
    qual->opname = strVal(lsecond(triple));
    qual->value = (Const *)lthird(triple);
    qual->rinfo = NULL;

    result = lappend(result, qual);
  }

  return result;
}

/*
 * ---------- Column pruning ----------
 *
 * Collects the 1-based attnums actually needed for a scan -- referenced in
 * the target list or in any restriction clause (pushed down or not; local
 * recheck clauses need the column's real value, and it's harmless to also
 * include columns only used by a pushed-down qual) -- as a plain List of
 * int (T_IntList), which is already a proper Node-safe list and needs no
 * extra wrapping to go straight into ForeignScan.fdw_private, unlike the
 * heterogeneous qual triples above. A whole-row reference (e.g. `SELECT
 * foreign_table FROM foreign_table`) resolves to every column, since
 * nothing is being pruned in that case.
 */

static List *pljs_fdw_collect_needed_attrs(RelOptInfo *baserel,
                                           TupleDesc tupdesc) {
  Bitmapset *attrs_used = NULL;

  pull_varattnos((Node *)baserel->reltarget->exprs, baserel->relid,
                 &attrs_used);

  ListCell *lc;
  foreach (lc, baserel->baserestrictinfo) {
    RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
    pull_varattnos((Node *)rinfo->clause, baserel->relid, &attrs_used);
  }

  bool need_all = bms_is_member(
      0 - FirstLowInvalidHeapAttributeNumber, attrs_used);
  List *result = NIL;

  for (int i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

    if (attr->attisdropped) {
      continue;
    }

    if (need_all ||
        bms_is_member(attr->attnum - FirstLowInvalidHeapAttributeNumber,
                      attrs_used)) {
      result = lappend_int(result, attr->attnum);
    }
  }

  return result;
}

/**
 * @brief Builds the `[column-name, ...]` array passed as the `columns`
 * argument to execute()/estimate(), from a #pljs_fdw_collect_needed_attrs
 * result.
 */
static JSValue pljs_fdw_needed_attrs_to_jsvalue(JSContext *ctx,
                                                List *needed_attnums,
                                                TupleDesc tupdesc) {
  JSValue columns = JS_NewArray(ctx);
  uint32_t idx = 0;
  ListCell *lc;

  foreach (lc, needed_attnums) {
    int attnum = lfirst_int(lc);
    Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

    JS_SetPropertyUint32(ctx, columns, idx++,
                         JS_NewString(ctx, NameStr(attr->attname)));
  }

  return columns;
}

/*
 * ---------- Column mapping ----------
 *
 * A column's local Postgres name and the underlying data source's own
 * field/column name don't always match (e.g. wrapping a REST API whose
 * JSON keys are `cust_id`/`cust_name` under Postgres columns named
 * `id`/`name`). Rather than changing what `columns`/quals/rows are keyed
 * by everywhere (a breaking change to every contract built so far),
 * mapping is exposed as one additional, purely informational argument:
 * `columnMap`, a `{localName: remoteName, ...}` object covering only the
 * columns whose standard `column_name` option (settable via `ALTER
 * FOREIGN TABLE ... ALTER COLUMN col OPTIONS (column_name '...')`, the
 * same option name postgres_fdw uses for the same purpose) differs from
 * the actual column name. execute()/estimate() rows/objects are still
 * always keyed by the local Postgres name -- pljs_jsvalue_to_datums
 * (types.c, unmodified) only ever looks up by that name -- columnMap just
 * lets a JS implementation look up what to call the remote field without
 * hardcoding that translation inline. A purely a runtime catalog lookup
 * (GetForeignColumnOptions, keyed by relid+attnum), unlike quals/
 * sortkeys: no planning-time capture or fdw_private threading needed,
 * since it doesn't depend on anything the planner computed.
 */

static char *pljs_fdw_get_column_name(Oid foreigntableid, AttrNumber attnum) {
  return pljs_fdw_get_option(GetForeignColumnOptions(foreigntableid, attnum),
                             "column_name");
}

static JSValue pljs_fdw_column_map_to_jsvalue(JSContext *ctx,
                                              Oid foreigntableid,
                                              List *needed_attnums,
                                              TupleDesc tupdesc) {
  JSValue obj = JS_NewObject(ctx);
  ListCell *lc;

  foreach (lc, needed_attnums) {
    int attnum = lfirst_int(lc);
    Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
    char *column_name = pljs_fdw_get_column_name(foreigntableid, attnum);

    if (column_name != NULL &&
        strcmp(column_name, NameStr(attr->attname)) != 0) {
      JS_SetPropertyStr(ctx, obj, NameStr(attr->attname),
                        JS_NewString(ctx, column_name));
    }
  }

  return obj;
}

/*
 * ---------- Sort pushdown ----------
 *
 * Finds the longest leading prefix of root->query_pathkeys satisfiable by
 * plain columns of baserel with the type's normal (default btree opfamily)
 * comparison semantics -- deliberately conservative, like qual pushdown:
 * expressions, other relations, and non-default opclasses are left alone,
 * meaning the planner falls back to an explicit Sort. A prefix (rather than
 * requiring every pathkey to match) is still useful on its own: Postgres
 * can finish the job with an IncrementalSort over the remainder.
 *
 * Like qual pushdown, a claimed sort order is trusted, not verified: if
 * GetForeignPaths offers a path claiming rows come back in a given order,
 * execute() must actually honor the sortkeys it's given, or query results
 * will be silently wrong (no local Sort is added to catch a mismatch).
 */

typedef struct pljs_fdw_sortkey {
  AttrNumber attnum;
  bool descending;
  bool nulls_first;
} pljs_fdw_sortkey;

static List *pljs_fdw_match_pathkeys(PlannerInfo *root, RelOptInfo *baserel) {
  List *result = NIL;
  ListCell *lc;

  foreach (lc, root->query_pathkeys) {
    PathKey *pathkey = lfirst_node(PathKey, lc);
    EquivalenceClass *ec = pathkey->pk_eclass;

    if (ec->ec_has_volatile) {
      break;
    }

    Var *var = NULL;
    ListCell *lc2;

    foreach (lc2, ec->ec_members) {
      EquivalenceMember *em = lfirst_node(EquivalenceMember, lc2);

      if (em->em_is_child || !bms_equal(em->em_relids, baserel->relids)) {
        continue;
      }

      if (IsA(em->em_expr, Var)) {
        var = (Var *)em->em_expr;
      }

      break;
    }

    if (var == NULL || var->varattno < 1) {
      break;
    }

    // Only claim pushdown under the type's normal comparison semantics --
    // a pathkey using some other opfamily (e.g. a non-default opclass)
    // doesn't necessarily mean what a plain <op>/>op> qual would.
    Oid default_opclass = GetDefaultOpClass(var->vartype, BTREE_AM_OID);

    if (!OidIsValid(default_opclass) ||
        get_opclass_family(default_opclass) != pathkey->pk_opfamily) {
      break;
    }

    bool descending;

    if (pathkey->pk_cmptype == COMPARE_LT) {
      descending = false;
    } else if (pathkey->pk_cmptype == COMPARE_GT) {
      descending = true;
    } else {
      break;
    }

    pljs_fdw_sortkey *sk =
        (pljs_fdw_sortkey *)palloc(sizeof(pljs_fdw_sortkey));

    sk->attnum = var->varattno;
    sk->descending = descending;
    sk->nulls_first = pathkey->pk_nulls_first;

    result = lappend(result, sk);
  }

  return result;
}

/**
 * @brief Encodes a #pljs_fdw_sortkey list as a flat, Node-safe T_IntList
 * (3 ints per entry: attnum, descending, nulls_first) for a ForeignPath's
 * fdw_private -- Path trees, like plan trees, can be copied, so this can't
 * hold raw pljs_fdw_sortkey structs directly.
 */
static List *pljs_fdw_sortkeys_to_plan_private(List *sortkeys) {
  List *result = NIL;
  ListCell *lc;

  foreach (lc, sortkeys) {
    pljs_fdw_sortkey *sk = (pljs_fdw_sortkey *)lfirst(lc);

    result = lappend_int(result, sk->attnum);
    result = lappend_int(result, sk->descending ? 1 : 0);
    result = lappend_int(result, sk->nulls_first ? 1 : 0);
  }

  return result;
}

/**
 * @brief Converts an encoded sortkeys list (see
 * #pljs_fdw_sortkeys_to_plan_private) to the `sortkeys` array passed to a
 * JS execute() call: `[{column, direction, nullsFirst}, ...]`.
 */
static JSValue pljs_fdw_sortkeys_to_jsvalue(JSContext *ctx,
                                            List *sortkeys_private,
                                            TupleDesc tupdesc) {
  JSValue arr = JS_NewArray(ctx);
  uint32_t idx = 0;
  int n = list_length(sortkeys_private);

  for (int i = 0; i + 2 < n; i += 3) {
    int attnum = list_nth_int(sortkeys_private, i);
    bool descending = list_nth_int(sortkeys_private, i + 1) != 0;
    bool nulls_first = list_nth_int(sortkeys_private, i + 2) != 0;
    Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

    JSValue obj = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, obj, "column",
                      JS_NewString(ctx, NameStr(attr->attname)));
    JS_SetPropertyStr(ctx, obj, "direction",
                      JS_NewString(ctx, descending ? "desc" : "asc"));
    JS_SetPropertyStr(ctx, obj, "nullsFirst", JS_NewBool(ctx, nulls_first));

    JS_SetPropertyUint32(ctx, arr, idx++, obj);
  }

  return arr;
}

/*
 * ---------- Scan execution ----------
 */

typedef struct pljs_fdw_scan_state {
  JSContext *ctx;
  JSValue instance;
  JSValue iterator;
  List *quals;          // pljs_fdw_qual*, decoded once in BeginForeignScan
                        // and reused by ReScanForeignScan
  List *needed_attnums;  // int, likewise decoded once and reused
  List *sortkeys_private; // int triples (see pljs_fdw_sortkeys_to_plan_private),
                         // likewise decoded once and reused
} pljs_fdw_scan_state;

/**
 * @brief Calls `execute(quals, columns, sortkeys, columnMap)` on an FDW
 * instance.
 *
 * @p quals, @p columns, @p sortkeys, and @p column_map are all
 * caller-built (see #pljs_fdw_quals_to_jsvalue,
 * #pljs_fdw_needed_attrs_to_jsvalue, #pljs_fdw_sortkeys_to_jsvalue,
 * #pljs_fdw_column_map_to_jsvalue) and their ownership passed in -- freed
 * here regardless of outcome.
 *
 * @returns The JS iterator returned by execute(), or a JS exception value
 *          (check with #JS_IsException) on failure.
 */
static JSValue pljs_fdw_call_execute(JSContext *ctx, JSValue instance,
                                     JSValue quals, JSValue columns,
                                     JSValue sortkeys, JSValue column_map) {
  JSValue execute_fn = JS_GetPropertyStr(ctx, instance, "execute");
  JSValueConst args[] = {quals, columns, sortkeys, column_map};

  JSValue iterator = JS_Call(ctx, execute_fn, instance, 4, args);

  JS_FreeValue(ctx, execute_fn);
  JS_FreeValue(ctx, quals);
  JS_FreeValue(ctx, columns);
  JS_FreeValue(ctx, sortkeys);
  JS_FreeValue(ctx, column_map);

  return iterator;
}

/**
 * @brief Fills a slot's values/isnull arrays (and marks it valid) from a JS
 * row object, via the existing pljs_jsvalue_to_datums (types.c). A key the
 * object doesn't have is left NULL, safe as long as nothing downstream
 * needs it (true for a pruned column; also true here for INSERT/UPDATE
 * columns the JS side didn't choose to echo back). Leaves @p slot cleared
 * (not stored) if @p value is JS null/undefined.
 *
 * Shared by IterateForeignScan (a yielded row) and
 * ExecForeignInsert/ExecForeignUpdate (an insert()/update() return value
 * used to reflect server-computed columns back, e.g. a generated id).
 */
static void pljs_fdw_apply_jsvalue_to_slot(JSContext *ctx, JSValue value,
                                           TupleTableSlot *slot) {
  TupleDesc tupdesc = slot->tts_tupleDescriptor;
  bool *nulls = (bool *)palloc0(sizeof(bool) * tupdesc->natts);
  Datum *values = pljs_jsvalue_to_datums(NULL, value, &nulls, tupdesc, ctx);

  if (values != NULL) {
    memcpy(slot->tts_values, values, sizeof(Datum) * tupdesc->natts);
    memcpy(slot->tts_isnull, nulls, sizeof(bool) * tupdesc->natts);
    ExecStoreVirtualTuple(slot);
  }
}

/**
 * @brief Converts a slot's current values into a JS row object
 * (`{column: value, ...}`), for INSERT/UPDATE's insert()/update() calls.
 * The inverse of #pljs_fdw_apply_jsvalue_to_slot.
 */
static JSValue pljs_fdw_slot_to_jsvalue(JSContext *ctx, TupleTableSlot *slot) {
  slot_getallattrs(slot);

  TupleDesc tupdesc = slot->tts_tupleDescriptor;
  JSValue obj = JS_NewObject(ctx);

  for (int i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

    if (attr->attisdropped) {
      continue;
    }

    JSValue val = pljs_datum_to_jsvalue(attr->atttypid, slot->tts_values[i],
                                        slot->tts_isnull[i], false, ctx);

    JS_SetPropertyStr(ctx, obj, NameStr(attr->attname), val);
  }

  return obj;
}

/**
 * @brief Frees a scan's JS-side state (the iterator) and clears @p node's
 * fdw_state.
 *
 * Only the iterator is scan-owned; @c ctx and @c instance belong to the FDW
 * context cache and persist across scans, so they are deliberately not
 * freed here.
 *
 * Safe to call with a NULL @c fdw_state (no-op) and safe to call more than
 * once. Used both by #pljs_fdw_end_foreign_scan and by every error path in
 * #pljs_fdw_iterate_foreign_scan / #pljs_fdw_rescan_foreign_scan, since
 * PostgreSQL does not guarantee EndForeignScan runs after a (sub)transaction
 * abort -- resources must be released before we ereport(ERROR), not after.
 */
static void pljs_fdw_free_scan_state(ForeignScanState *node) {
  pljs_fdw_scan_state *state = (pljs_fdw_scan_state *)node->fdw_state;

  if (state == NULL) {
    return;
  }

  JS_FreeValue(state->ctx, state->iterator);

  node->fdw_state = NULL;
}

#define PLJS_FDW_DEFAULT_ROWS 1000

/**
 * @brief Planning-time scratch data stashed in baserel->fdw_private,
 * computed once in GetForeignRelSize and reused by GetForeignPlan.
 */
typedef struct pljs_fdw_rel_private {
  List *pushable_quals; // pljs_fdw_qual*
  List *needed_attnums; // int; see pljs_fdw_collect_needed_attrs
} pljs_fdw_rel_private;

/**
 * @brief Estimates row count and computes pushable quals/needed columns
 * for a foreign table scan.
 *
 * Both are stashed in baserel->fdw_private for GetForeignPlan to reuse. If
 * the FDW class implements an optional `estimate(quals, columns,
 * columnMap)` method,
 * it's called (with the same JS instance BeginForeignScan will reuse -- see
 * #pljs_fdw_get_or_create_instance) and its return value used as the row
 * estimate; otherwise PLJS_FDW_DEFAULT_ROWS is used. This runs during
 * planning, so it happens even for a plain EXPLAIN without ANALYZE (whereas
 * BeginForeignScan skips setup for that case) -- necessary, since there's
 * no other way to get a real estimate.
 */
static void pljs_fdw_get_foreign_rel_size(PlannerInfo *root,
                                          RelOptInfo *baserel,
                                          Oid foreigntableid) {
  Relation rel = table_open(foreigntableid, NoLock);
  TupleDesc tupdesc = RelationGetDescr(rel);

  pljs_fdw_rel_private *priv =
      (pljs_fdw_rel_private *)palloc(sizeof(pljs_fdw_rel_private));

  priv->pushable_quals =
      pljs_fdw_extract_pushable_quals(baserel->baserestrictinfo,
                                      baserel->relid);
  priv->needed_attnums = pljs_fdw_collect_needed_attrs(baserel, tupdesc);
  baserel->fdw_private = (void *)priv;

  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(foreigntableid, &ctx, &instance);

  double rows = PLJS_FDW_DEFAULT_ROWS;
  JSValue estimate_fn = JS_GetPropertyStr(ctx, instance, "estimate");

  if (JS_IsFunction(ctx, estimate_fn)) {
    JSValue quals_js =
        pljs_fdw_quals_to_jsvalue(ctx, priv->pushable_quals, tupdesc);
    JSValue columns_js =
        pljs_fdw_needed_attrs_to_jsvalue(ctx, priv->needed_attnums, tupdesc);
    JSValue column_map_js = pljs_fdw_column_map_to_jsvalue(
        ctx, foreigntableid, priv->needed_attnums, tupdesc);
    JSValueConst args[] = {quals_js, columns_js, column_map_js};

    JSValue result = JS_Call(ctx, estimate_fn, instance, 3, args);

    JS_FreeValue(ctx, quals_js);
    JS_FreeValue(ctx, columns_js);
    JS_FreeValue(ctx, column_map_js);

    if (JS_IsException(result)) {
      char *error_message = pljs_dump_error(ctx);
      JS_FreeValue(ctx, estimate_fn);
      table_close(rel, NoLock);
      ereport(ERROR,
              (errmsg("pljs_fdw: estimate() failed for foreign table %u",
                      foreigntableid),
               errdetail("%s", error_message)));
    }

    double estimated;
    JS_ToFloat64(ctx, &estimated, result);
    JS_FreeValue(ctx, result);

    if (estimated >= 1) {
      rows = estimated;
    }
  }

  JS_FreeValue(ctx, estimate_fn);
  table_close(rel, NoLock);

  baserel->rows = rows;
}

static void pljs_fdw_get_foreign_paths(PlannerInfo *root, RelOptInfo *baserel,
                                       Oid foreigntableid) {
  ForeignPath *path = create_foreignscan_path(
      root, baserel, NULL /* default pathtarget */, baserel->rows,
#if PG_VERSION_NUM >= 180000
      0 /* disabled_nodes */,
#endif
      0 /* startup_cost */, baserel->rows /* total_cost */, NIL /* pathkeys */,
      baserel->lateral_relids, NULL /* fdw_outerpath */,
      NIL /* fdw_restrictinfo */, NIL /* fdw_private */);

  add_path(baserel, (Path *)path);

  // If a leading prefix of the query's desired sort order matches plain
  // columns of this table, offer an additional path claiming that order --
  // same cost as the unsorted path (see the "Sort pushdown" section
  // comment), so the planner can skip an explicit Sort/IncrementalSort
  // when it helps, without it being forced when it doesn't.
  List *sortkeys = pljs_fdw_match_pathkeys(root, baserel);

  if (sortkeys != NIL) {
    List *pathkeys = list_copy_head(root->query_pathkeys,
                                    list_length(sortkeys));
    List *sortkeys_private = pljs_fdw_sortkeys_to_plan_private(sortkeys);

    ForeignPath *sorted_path = create_foreignscan_path(
        root, baserel, NULL /* default pathtarget */, baserel->rows,
#if PG_VERSION_NUM >= 180000
        0 /* disabled_nodes */,
#endif
        0 /* startup_cost */, baserel->rows /* total_cost */, pathkeys,
        baserel->lateral_relids, NULL /* fdw_outerpath */,
        NIL /* fdw_restrictinfo */, sortkeys_private);

    add_path(baserel, (Path *)sorted_path);
  }
}

static ForeignScan *pljs_fdw_get_foreign_plan(PlannerInfo *root,
                                              RelOptInfo *baserel,
                                              Oid foreigntableid,
                                              ForeignPath *best_path,
                                              List *tlist, List *scan_clauses,
                                              Plan *outer_plan) {
  pljs_fdw_rel_private *priv = (pljs_fdw_rel_private *)baserel->fdw_private;
  List *pushable_rinfos = NIL;
  ListCell *lc;

  foreach (lc, priv->pushable_quals) {
    pljs_fdw_qual *qual = (pljs_fdw_qual *)lfirst(lc);
    pushable_rinfos = lappend(pushable_rinfos, qual->rinfo);
  }

  // Local recheck clauses are everything NOT pushed down. Quals that ARE
  // pushed down are trusted to have been applied by execute() and are
  // deliberately excluded here -- that's what makes this real pushdown
  // rather than just a hint (see the "Qual pushdown" section comment).
  List *local_clauses = NIL;

  foreach (lc, scan_clauses) {
    RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

    if (!list_member_ptr(pushable_rinfos, rinfo)) {
      local_clauses = lappend(local_clauses, rinfo);
    }
  }

  List *qpqual = extract_actual_clauses(local_clauses, false);
  List *qual_private = pljs_fdw_quals_to_plan_private(priv->pushable_quals);

  // best_path->fdw_private is only non-NIL for the sorted path variant
  // built in GetForeignPaths (see pljs_fdw_sortkeys_to_plan_private); NIL
  // here just means "no sort claimed," decoding to an empty JS array.
  List *sortkeys_private = best_path->fdw_private;

  // fdw_private[0] = qual triples (see pljs_fdw_quals_to_plan_private),
  // fdw_private[1] = needed_attnums (already a plain, Node-safe T_IntList),
  // fdw_private[2] = sortkeys_private (ditto).
  List *fdw_private =
      list_make3(qual_private, priv->needed_attnums, sortkeys_private);

  return make_foreignscan(tlist, qpqual, baserel->relid, NIL /* fdw_exprs */,
                          fdw_private, NIL /* fdw_scan_tlist */,
                          NIL /* fdw_recheck_quals */, outer_plan);
}

static void pljs_fdw_begin_foreign_scan(ForeignScanState *node, int eflags) {
  // Nothing external to set up for a plain EXPLAIN (no ANALYZE).
  if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
    return;
  }

  Oid foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);

  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(foreigntableid, &ctx, &instance);

  ForeignScan *plan = (ForeignScan *)node->ss.ps.plan;
  List *quals = pljs_fdw_quals_from_plan_private(linitial(plan->fdw_private));
  List *needed_attnums = (List *)lsecond(plan->fdw_private);
  List *sortkeys_private = (List *)lthird(plan->fdw_private);
  TupleDesc tupdesc = node->ss.ss_currentRelation->rd_att;

  JSValue quals_js = pljs_fdw_quals_to_jsvalue(ctx, quals, tupdesc);
  JSValue columns_js =
      pljs_fdw_needed_attrs_to_jsvalue(ctx, needed_attnums, tupdesc);
  JSValue sortkeys_js =
      pljs_fdw_sortkeys_to_jsvalue(ctx, sortkeys_private, tupdesc);
  JSValue column_map_js = pljs_fdw_column_map_to_jsvalue(
      ctx, foreigntableid, needed_attnums, tupdesc);
  JSValue iterator = pljs_fdw_call_execute(
      ctx, instance, quals_js, columns_js, sortkeys_js, column_map_js);

  if (JS_IsException(iterator)) {
    char *error_message = pljs_dump_error(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: execute() failed for foreign table %u",
                           foreigntableid),
                    errdetail("%s", error_message)));
  }

  pljs_fdw_scan_state *state =
      (pljs_fdw_scan_state *)palloc(sizeof(pljs_fdw_scan_state));

  state->ctx = ctx;
  state->instance = instance;
  state->iterator = iterator;
  state->quals = quals;
  state->needed_attnums = needed_attnums;
  state->sortkeys_private = sortkeys_private;

  node->fdw_state = state;
}

static TupleTableSlot *pljs_fdw_iterate_foreign_scan(ForeignScanState *node) {
  TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
  pljs_fdw_scan_state *state = (pljs_fdw_scan_state *)node->fdw_state;

  ExecClearTuple(slot);

  if (state == NULL) {
    return slot;
  }

  JSContext *ctx = state->ctx;
  JSValue next_fn = JS_GetPropertyStr(ctx, state->iterator, "next");
  JSValue result = JS_Call(ctx, next_fn, state->iterator, 0, NULL);

  JS_FreeValue(ctx, next_fn);

  if (JS_IsException(result)) {
    char *error_message = pljs_dump_error(ctx);
    pljs_fdw_free_scan_state(node);
    ereport(ERROR, (errmsg("pljs_fdw: error iterating foreign scan"),
                    errdetail("%s", error_message)));
  }

  JSValue done_val = JS_GetPropertyStr(ctx, result, "done");
  bool done = (bool)JS_ToBool(ctx, done_val);
  JS_FreeValue(ctx, done_val);

  if (done) {
    JS_FreeValue(ctx, result);
    return slot;
  }

  JSValue value = JS_GetPropertyStr(ctx, result, "value");
  JS_FreeValue(ctx, result);

  pljs_fdw_apply_jsvalue_to_slot(ctx, value, slot);
  JS_FreeValue(ctx, value);

  return slot;
}

static void pljs_fdw_rescan_foreign_scan(ForeignScanState *node) {
  pljs_fdw_scan_state *state = (pljs_fdw_scan_state *)node->fdw_state;

  if (state == NULL) {
    return;
  }

  JS_FreeValue(state->ctx, state->iterator);

  TupleDesc tupdesc = node->ss.ss_currentRelation->rd_att;
  Oid foreigntableid = RelationGetRelid(node->ss.ss_currentRelation);
  JSValue quals_js =
      pljs_fdw_quals_to_jsvalue(state->ctx, state->quals, tupdesc);
  JSValue columns_js = pljs_fdw_needed_attrs_to_jsvalue(
      state->ctx, state->needed_attnums, tupdesc);
  JSValue sortkeys_js = pljs_fdw_sortkeys_to_jsvalue(
      state->ctx, state->sortkeys_private, tupdesc);
  JSValue column_map_js = pljs_fdw_column_map_to_jsvalue(
      state->ctx, foreigntableid, state->needed_attnums, tupdesc);
  state->iterator =
      pljs_fdw_call_execute(state->ctx, state->instance, quals_js,
                            columns_js, sortkeys_js, column_map_js);

  if (JS_IsException(state->iterator)) {
    char *error_message = pljs_dump_error(state->ctx);
    pljs_fdw_free_scan_state(node);
    ereport(ERROR, (errmsg("pljs_fdw: execute() failed on rescan"),
                    errdetail("%s", error_message)));
  }
}

static void pljs_fdw_end_foreign_scan(ForeignScanState *node) {
  pljs_fdw_free_scan_state(node);
}

/*
 * ---------- Write support ----------
 *
 * A foreign table's class opts in to writes per-operation, by defining the
 * relevant JS members:
 *
 *   insert(row) { ... return updated row, or nothing }
 *   get rowIdColumn() { return 'id'; }  // required for update()/delete()
 *   update(rowId, row) { ... return updated row, or nothing }
 *   delete(rowId) { ... }
 *
 * insert() alone is enough for INSERT support -- there's no existing row to
 * identify. update()/delete() additionally require rowIdColumn, naming an
 * existing column Postgres re-fetches per modified row (via Postgres's
 * standard "row identity" plan machinery, add_row_identity_var) and passes
 * back as each call's first argument. If insert()/update() return an
 * object, its values overwrite the affected row's (e.g. to reflect a
 * server-computed id or default) -- returning nothing leaves the executor's
 * own values as given.
 *
 * The row-id VALUE and TYPE are looked up separately, both once in
 * BeginForeignModify (see pljs_fdw_modify_state): the OID (needed for
 * pljs_datum_to_jsvalue) by name from the target relation's tupdesc; the
 * junk column's position within planSlot -- not a fixed/predictable
 * attnum, since AddForeignUpdateTargets's row-identity var is appended to
 * the scan subplan's target list -- via ExecFindJunkAttributeInTlist,
 * matching how postgres_fdw itself locates its own "ctid" junk column.
 */

typedef struct pljs_fdw_modify_state {
  JSContext *ctx;
  JSValue instance;
  Oid rowid_typid;             // InvalidOid if this table has no rowIdColumn
  AttrNumber rowid_junk_attno; // position of the row-identity value within
                               // planSlot for UPDATE/DELETE; InvalidAttrNumber
                               // otherwise (see pljs_fdw_begin_foreign_modify)
} pljs_fdw_modify_state;

/**
 * @brief Reads the FDW instance's optional `rowIdColumn` string property.
 * @returns A palloc'd copy of the column name, or NULL if unset/not a
 * string.
 */
static char *pljs_fdw_get_rowid_column(JSContext *ctx, JSValue instance) {
  JSValue val = JS_GetPropertyStr(ctx, instance, "rowIdColumn");
  char *result = NULL;

  if (JS_IsString(val)) {
    const char *s = JS_ToCString(ctx, val);
    result = pstrdup(s);
    JS_FreeCString(ctx, s);
  }

  JS_FreeValue(ctx, val);

  return result;
}

/**
 * @brief Advertises which of INSERT/UPDATE/DELETE this foreign table
 * supports, based on which of insert()/rowIdColumn+update()/
 * rowIdColumn+delete() the FDW class defines.
 */
static int pljs_fdw_is_foreign_rel_updatable(Relation rel) {
  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(RelationGetRelid(rel), &ctx, &instance);

  int result = 0;

  JSValue insert_fn = JS_GetPropertyStr(ctx, instance, "insert");
  if (JS_IsFunction(ctx, insert_fn)) {
    result |= (1 << CMD_INSERT);
  }
  JS_FreeValue(ctx, insert_fn);

  char *rowid_column = pljs_fdw_get_rowid_column(ctx, instance);

  if (rowid_column != NULL) {
    JSValue update_fn = JS_GetPropertyStr(ctx, instance, "update");
    if (JS_IsFunction(ctx, update_fn)) {
      result |= (1 << CMD_UPDATE);
    }
    JS_FreeValue(ctx, update_fn);

    JSValue delete_fn = JS_GetPropertyStr(ctx, instance, "delete");
    if (JS_IsFunction(ctx, delete_fn)) {
      result |= (1 << CMD_DELETE);
    }
    JS_FreeValue(ctx, delete_fn);
  }

  // updateWhere()/deleteWhere() (direct modify) grant the same capability
  // independent of rowIdColumn -- they operate on a whole matching set at
  // once, with no per-row identity needed. A class can offer either path,
  // both, or neither.
  JSValue update_where_fn = JS_GetPropertyStr(ctx, instance, "updateWhere");
  if (JS_IsFunction(ctx, update_where_fn)) {
    result |= (1 << CMD_UPDATE);
  }
  JS_FreeValue(ctx, update_where_fn);

  JSValue delete_where_fn = JS_GetPropertyStr(ctx, instance, "deleteWhere");
  if (JS_IsFunction(ctx, delete_where_fn)) {
    result |= (1 << CMD_DELETE);
  }
  JS_FreeValue(ctx, delete_where_fn);

  return result;
}

/**
 * @brief Registers the rowIdColumn (if any) as a row-identity junk column,
 * so its value is available in planSlot during ExecForeignUpdate/Delete.
 */
static void pljs_fdw_add_foreign_update_targets(PlannerInfo *root,
                                                Index rtindex,
                                                RangeTblEntry *target_rte,
                                                Relation target_relation) {
  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(RelationGetRelid(target_relation), &ctx,
                                  &instance);

  char *rowid_column = pljs_fdw_get_rowid_column(ctx, instance);

  if (rowid_column == NULL) {
    return;
  }

  TupleDesc tupdesc = RelationGetDescr(target_relation);
  Form_pg_attribute found_attr = NULL;

  for (int i = 0; i < tupdesc->natts; i++) {
    Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

    if (!attr->attisdropped &&
        strcmp(NameStr(attr->attname), rowid_column) == 0) {
      found_attr = attr;
      break;
    }
  }

  if (found_attr == NULL) {
    ereport(ERROR, (errmsg("pljs_fdw: rowIdColumn \"%s\" does not exist on "
                           "foreign table",
                           rowid_column)));
  }

  Var *var = makeVar(rtindex, found_attr->attnum, found_attr->atttypid,
                     found_attr->atttypmod, found_attr->attcollation, 0);

  add_row_identity_var(root, var, rtindex, rowid_column);
}

static void pljs_fdw_begin_foreign_modify(ModifyTableState *mtstate,
                                          ResultRelInfo *rinfo,
                                          List *fdw_private, int subplan_index,
                                          int eflags) {
  if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
    return;
  }

  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(RelationGetRelid(rinfo->ri_RelationDesc),
                                  &ctx, &instance);

  pljs_fdw_modify_state *mstate =
      (pljs_fdw_modify_state *)palloc(sizeof(pljs_fdw_modify_state));

  mstate->ctx = ctx;
  mstate->instance = instance;
  mstate->rowid_typid = InvalidOid;
  mstate->rowid_junk_attno = InvalidAttrNumber;

  if (mtstate->operation == CMD_UPDATE || mtstate->operation == CMD_DELETE) {
    char *rowid_column = pljs_fdw_get_rowid_column(ctx, instance);

    if (rowid_column != NULL) {
      TupleDesc tupdesc = RelationGetDescr(rinfo->ri_RelationDesc);

      for (int i = 0; i < tupdesc->natts; i++) {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

        if (!attr->attisdropped &&
            strcmp(NameStr(attr->attname), rowid_column) == 0) {
          mstate->rowid_typid = attr->atttypid;
          break;
        }
      }

      // The row-identity var AddForeignUpdateTargets registered doesn't
      // land at a fixed/predictable attnum -- it's an extra resjunk column
      // appended to the scan subplan's target list, found by the name we
      // gave it (matching postgres_fdw's own approach for its "ctid" junk
      // column).
      mstate->rowid_junk_attno = ExecFindJunkAttributeInTlist(
          outerPlanState(mtstate)->plan->targetlist, rowid_column);
    }
  }

  rinfo->ri_FdwState = mstate;
}

/**
 * @brief Sets up rinfo->ri_FdwState for `COPY foreign_table FROM ...`.
 *
 * COPY FROM doesn't go through BeginForeignModify at all -- it builds its
 * own synthetic ModifyTableState (operation = CMD_INSERT, plan = NULL) and
 * calls this dedicated entry point instead, before repeatedly calling the
 * very same ExecForeignInsert used for a plain `INSERT INTO ...`. Reusing
 * pljs_fdw_begin_foreign_modify here is safe specifically because its only
 * mtstate->plan dereference is on the CMD_UPDATE/CMD_DELETE branch, which
 * a CMD_INSERT operation (all COPY FROM ever is) never takes -- otherwise
 * this would crash on the NULL plan.
 */
static void pljs_fdw_begin_foreign_insert(ModifyTableState *mtstate,
                                          ResultRelInfo *rinfo) {
  pljs_fdw_begin_foreign_modify(mtstate, rinfo, NIL, 0, 0);
}

static TupleTableSlot *pljs_fdw_exec_foreign_insert(EState *estate,
                                                    ResultRelInfo *rinfo,
                                                    TupleTableSlot *slot,
                                                    TupleTableSlot *planSlot) {
  pljs_fdw_modify_state *mstate =
      (pljs_fdw_modify_state *)rinfo->ri_FdwState;
  JSContext *ctx = mstate->ctx;

  JSValue insert_fn = JS_GetPropertyStr(ctx, mstate->instance, "insert");

  if (!JS_IsFunction(ctx, insert_fn)) {
    JS_FreeValue(ctx, insert_fn);
    ereport(ERROR, (errmsg("pljs_fdw: foreign table does not support INSERT "
                           "(no insert() method)")));
  }

  JSValue row = pljs_fdw_slot_to_jsvalue(ctx, slot);
  JSValueConst args[] = {row};
  JSValue result = JS_Call(ctx, insert_fn, mstate->instance, 1, args);

  JS_FreeValue(ctx, insert_fn);
  JS_FreeValue(ctx, row);

  if (JS_IsException(result)) {
    char *error_message = pljs_dump_error(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: insert() failed"),
                    errdetail("%s", error_message)));
  }

  if (JS_IsObject(result) && !JS_IsNull(result)) {
    pljs_fdw_apply_jsvalue_to_slot(ctx, result, slot);
  }

  JS_FreeValue(ctx, result);

  return slot;
}

// No batch counterpart exists for UPDATE/DELETE, deliberately: Postgres's
// FdwRoutine API itself only defines ExecForeignBatchInsert/
// GetForeignModifyBatchSize -- there's no ExecForeignBatchUpdate/Delete to
// wire up. The executor always calls ExecForeignUpdate/ExecForeignDelete
// one row at a time, with no hook to receive several at once the way
// ExecForeignBatchInsert does. Batching that path ourselves would mean
// building a custom buffering layer with no Postgres-blessed reference to
// check against (unlike everything else in this file) and real open
// questions -- when to flush, and how that interacts with RETURNING,
// triggers, or a self-referential UPDATE. The direct-modify path
// (updateWhere()/deleteWhere(), see "Direct modify" above) already covers
// the equivalent for whichever UPDATE/DELETE statements are eligible --
// a fully pushable WHERE clause, and for UPDATE, only constant SET
// values -- performing the whole statement in one call rather than a
// per-row one; that's as far as batching UPDATE/DELETE goes here.

#define PLJS_FDW_DEFAULT_BATCH_SIZE 100

/**
 * @brief Determines how many rows a multi-row INSERT/COPY FROM should
 * batch together into one ExecForeignBatchInsert() call.
 *
 * Returns 1 (no batching -- falls back to plain ExecForeignInsert, one
 * call per row) unless the class defines `insertMany(rows)`. Also returns
 * 1 whenever RETURNING, WITH CHECK OPTION, or a BEFORE/AFTER ROW INSERT
 * trigger is involved, mirroring postgres_fdw's own
 * postgresGetForeignModifyBatchSize: RETURNING would need each row's
 * server-computed values correctly matched back by position, and a BEFORE
 * ROW trigger could depend on rows being processed one at a time (e.g.
 * querying what's been inserted so far) -- neither is worth the added
 * complexity to support correctly here.
 */
static int pljs_fdw_get_foreign_modify_batch_size(ResultRelInfo *rinfo) {
  if (rinfo->ri_projectReturning != NULL || rinfo->ri_WithCheckOptions != NIL ||
      (rinfo->ri_TrigDesc && (rinfo->ri_TrigDesc->trig_insert_before_row ||
                              rinfo->ri_TrigDesc->trig_insert_after_row))) {
    return 1;
  }

  pljs_fdw_modify_state *mstate = (pljs_fdw_modify_state *)rinfo->ri_FdwState;
  JSContext *ctx;
  JSValue instance;

  if (mstate != NULL) {
    ctx = mstate->ctx;
    instance = mstate->instance;
  } else {
    // EXPLAIN without ANALYZE: BeginForeignModify returned early, so
    // ri_FdwState is still NULL, but we're still asked for a batch size.
    pljs_fdw_get_or_create_instance(RelationGetRelid(rinfo->ri_RelationDesc),
                                    &ctx, &instance);
  }

  JSValue insert_many_fn = JS_GetPropertyStr(ctx, instance, "insertMany");
  bool has_insert_many = JS_IsFunction(ctx, insert_many_fn);
  JS_FreeValue(ctx, insert_many_fn);

  return has_insert_many ? PLJS_FDW_DEFAULT_BATCH_SIZE : 1;
}

static TupleTableSlot **pljs_fdw_exec_foreign_batch_insert(
    EState *estate, ResultRelInfo *rinfo, TupleTableSlot **slots,
    TupleTableSlot **planSlots, int *numSlots) {
  pljs_fdw_modify_state *mstate =
      (pljs_fdw_modify_state *)rinfo->ri_FdwState;
  JSContext *ctx = mstate->ctx;

  JSValue insert_many_fn =
      JS_GetPropertyStr(ctx, mstate->instance, "insertMany");

  if (!JS_IsFunction(ctx, insert_many_fn)) {
    JS_FreeValue(ctx, insert_many_fn);
    ereport(ERROR, (errmsg("pljs_fdw: foreign table does not support batch "
                           "INSERT (no insertMany() method)")));
  }

  JSValue rows = JS_NewArray(ctx);

  for (int i = 0; i < *numSlots; i++) {
    JSValue row = pljs_fdw_slot_to_jsvalue(ctx, slots[i]);
    JS_SetPropertyUint32(ctx, rows, i, row);
  }

  JSValueConst args[] = {rows};
  JSValue result = JS_Call(ctx, insert_many_fn, mstate->instance, 1, args);

  JS_FreeValue(ctx, insert_many_fn);
  JS_FreeValue(ctx, rows);

  if (JS_IsException(result)) {
    char *error_message = pljs_dump_error(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: insertMany() failed"),
                    errdetail("%s", error_message)));
  }

  // If insertMany() returned an array, apply each element back to its
  // corresponding slot by position (mirroring insert()'s single-row
  // behavior) -- e.g. to reflect server-generated ids.
  if (JS_IsArray(ctx, result)) {
    uint32_t result_len = pljs_js_array_length(result, ctx);
    uint32_t apply_count = Min(result_len, (uint32_t)*numSlots);

    for (uint32_t i = 0; i < apply_count; i++) {
      JSValue row_result = JS_GetPropertyUint32(ctx, result, i);

      if (JS_IsObject(row_result) && !JS_IsNull(row_result)) {
        pljs_fdw_apply_jsvalue_to_slot(ctx, row_result, slots[i]);
      }

      JS_FreeValue(ctx, row_result);
    }
  }

  JS_FreeValue(ctx, result);

  return slots;
}

static TupleTableSlot *pljs_fdw_exec_foreign_update(EState *estate,
                                                    ResultRelInfo *rinfo,
                                                    TupleTableSlot *slot,
                                                    TupleTableSlot *planSlot) {
  pljs_fdw_modify_state *mstate =
      (pljs_fdw_modify_state *)rinfo->ri_FdwState;
  JSContext *ctx = mstate->ctx;

  if (mstate->rowid_junk_attno == InvalidAttrNumber ||
      !OidIsValid(mstate->rowid_typid)) {
    ereport(ERROR, (errmsg("pljs_fdw: foreign table does not support UPDATE "
                           "(no rowIdColumn)")));
  }

  bool isnull;
  Datum rowid_datum =
      ExecGetJunkAttribute(planSlot, mstate->rowid_junk_attno, &isnull);
  JSValue rowid_js =
      pljs_datum_to_jsvalue(mstate->rowid_typid, rowid_datum, isnull, false,
                            ctx);

  JSValue update_fn = JS_GetPropertyStr(ctx, mstate->instance, "update");

  if (!JS_IsFunction(ctx, update_fn)) {
    JS_FreeValue(ctx, update_fn);
    JS_FreeValue(ctx, rowid_js);
    ereport(ERROR, (errmsg("pljs_fdw: foreign table does not support UPDATE "
                           "(no update() method)")));
  }

  JSValue row = pljs_fdw_slot_to_jsvalue(ctx, slot);
  JSValueConst args[] = {rowid_js, row};
  JSValue result = JS_Call(ctx, update_fn, mstate->instance, 2, args);

  JS_FreeValue(ctx, update_fn);
  JS_FreeValue(ctx, rowid_js);
  JS_FreeValue(ctx, row);

  if (JS_IsException(result)) {
    char *error_message = pljs_dump_error(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: update() failed"),
                    errdetail("%s", error_message)));
  }

  if (JS_IsObject(result) && !JS_IsNull(result)) {
    pljs_fdw_apply_jsvalue_to_slot(ctx, result, slot);
  }

  JS_FreeValue(ctx, result);

  return slot;
}

static TupleTableSlot *pljs_fdw_exec_foreign_delete(EState *estate,
                                                    ResultRelInfo *rinfo,
                                                    TupleTableSlot *slot,
                                                    TupleTableSlot *planSlot) {
  pljs_fdw_modify_state *mstate =
      (pljs_fdw_modify_state *)rinfo->ri_FdwState;
  JSContext *ctx = mstate->ctx;

  if (mstate->rowid_junk_attno == InvalidAttrNumber ||
      !OidIsValid(mstate->rowid_typid)) {
    ereport(ERROR, (errmsg("pljs_fdw: foreign table does not support DELETE "
                           "(no rowIdColumn)")));
  }

  bool isnull;
  Datum rowid_datum =
      ExecGetJunkAttribute(planSlot, mstate->rowid_junk_attno, &isnull);
  JSValue rowid_js =
      pljs_datum_to_jsvalue(mstate->rowid_typid, rowid_datum, isnull, false,
                            ctx);

  JSValue delete_fn = JS_GetPropertyStr(ctx, mstate->instance, "delete");

  if (!JS_IsFunction(ctx, delete_fn)) {
    JS_FreeValue(ctx, delete_fn);
    JS_FreeValue(ctx, rowid_js);
    ereport(ERROR, (errmsg("pljs_fdw: foreign table does not support DELETE "
                           "(no delete() method)")));
  }

  JSValueConst args[] = {rowid_js};
  JSValue result = JS_Call(ctx, delete_fn, mstate->instance, 1, args);

  JS_FreeValue(ctx, delete_fn);
  JS_FreeValue(ctx, rowid_js);

  if (JS_IsException(result)) {
    char *error_message = pljs_dump_error(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: delete() failed"),
                    errdetail("%s", error_message)));
  }

  JS_FreeValue(ctx, result);

  return slot;
}

/**
 * @brief Adds EXPLAIN VERBOSE detail for INSERT/UPDATE/DELETE on a foreign
 * table (and COPY FROM, which reaches here as CMD_INSERT too).
 *
 * Mirrors postgres_fdw's own postgresExplainForeignModify: gated on
 * es->verbose (nothing extra otherwise, matching every other EXPLAIN
 * output in core), and only shows "Batch Size" when it's actually
 * meaningful (rinfo->ri_BatchSize > 0 -- UPDATE/DELETE never batch, so it
 * stays unset for those). ri_BatchSize is already valid here even under a
 * plain EXPLAIN without ANALYZE, since GetForeignModifyBatchSize runs
 * regardless of ANALYZE to set it up.
 */
static void pljs_fdw_explain_foreign_modify(ModifyTableState *mtstate,
                                            ResultRelInfo *rinfo,
                                            List *fdw_private,
                                            int subplan_index,
                                            struct ExplainState *es) {
  if (!es->verbose) {
    return;
  }

  const char *op;

  switch (mtstate->operation) {
  case CMD_INSERT:
    op = "module-backed foreign insert";
    break;
  case CMD_UPDATE:
    op = "module-backed foreign update";
    break;
  case CMD_DELETE:
    op = "module-backed foreign delete";
    break;
  default:
    op = "module-backed foreign modify";
    break;
  }

  ExplainPropertyText("pljs FDW", op, es);

  if (rinfo->ri_BatchSize > 0) {
    ExplainPropertyInteger("Batch Size", NULL, rinfo->ri_BatchSize, es);
  }
}

/*
 * ---------- Direct modify ----------
 *
 * The per-row UPDATE/DELETE path (see "Write support" above) scans
 * matching rows one at a time and calls update()/delete() once per row.
 * Direct modify is an optional, more aggressive alternative: when a
 * WHERE clause is entirely pushable and (for UPDATE) every SET
 * assignment is a plain constant, Postgres can skip the per-row scan
 * entirely and let updateWhere(quals, values)/deleteWhere(quals) perform
 * the whole operation in one call, returning the number of affected rows.
 *
 * A class opts in independent of the per-row path -- rowIdColumn isn't
 * needed here, since there's no per-row identity to look up:
 *
 *   updateWhere(quals, values) { ... return count; }  // enables direct UPDATE
 *   deleteWhere(quals) { ... return count; }          // enables direct DELETE
 *
 * Deliberately narrow, matching the same conservative philosophy as qual/
 * sort pushdown: PlanDirectModify only offers this when nothing would be
 * lost by skipping the per-row path entirely -- no RETURNING (nothing to
 * fetch back per row), no BEFORE/AFTER ROW trigger (which could depend on
 * row-by-row processing), and no local recheck qual left un-pushed
 * (anything not expressible as a simple `column <op> constant`, or a SET
 * value that isn't a plain constant, falls back to the per-row path
 * instead of being silently dropped or mishandled).
 */

typedef struct pljs_fdw_direct_modify_state {
  JSContext *ctx;
  JSValue instance;
  CmdType operation;  // CMD_UPDATE or CMD_DELETE
  List *quals;        // pljs_fdw_qual*
  List *set_values;   // pljs_fdw_qual* (UPDATE only; NIL for DELETE)
  bool can_set_tag;   // whether to report the affected-row count upward
  bool executed;      // this scan only ever "iterates" once
} pljs_fdw_direct_modify_state;

/**
 * @brief Converts UPDATE's SET assignments to the `values` object passed
 * to updateWhere(): `{column: value, ...}`, as opposed to the
 * {column,operator,value} array shape used for quals.
 */
static JSValue pljs_fdw_set_values_to_jsvalue(JSContext *ctx, List *set_values,
                                              TupleDesc tupdesc) {
  JSValue obj = JS_NewObject(ctx);
  ListCell *lc;

  foreach (lc, set_values) {
    pljs_fdw_qual *sv = (pljs_fdw_qual *)lfirst(lc);
    Form_pg_attribute attr = TupleDescAttr(tupdesc, sv->attnum - 1);

    JS_SetPropertyStr(
        ctx, obj, NameStr(attr->attname),
        pljs_datum_to_jsvalue(sv->value->consttype, sv->value->constvalue,
                              sv->value->constisnull, false, ctx));
  }

  return obj;
}

/**
 * @brief Finds the ForeignScan that scans @p rtindex directly under a
 * ModifyTable, if any.
 *
 * Deliberately narrower than postgres_fdw's own find_modifytable_subplan:
 * only the plain "ForeignScan is the immediate child" case is handled,
 * not the Append/inheritance-partition case, since pljs_fdw doesn't
 * support partitioned tables as a target here.
 */
static ForeignScan *pljs_fdw_find_modifytable_subplan(ModifyTable *plan,
                                                       Index rtindex) {
  Plan *subplan = outerPlan(plan);

  if (subplan != NULL && IsA(subplan, ForeignScan)) {
    ForeignScan *fscan = (ForeignScan *)subplan;

    if (bms_is_member(rtindex, fscan->fs_base_relids)) {
      return fscan;
    }
  }

  return NULL;
}

static bool pljs_fdw_plan_direct_modify(PlannerInfo *root, ModifyTable *plan,
                                        Index resultRelation,
                                        int subplan_index) {
  CmdType operation = plan->operation;

  if (operation != CMD_UPDATE && operation != CMD_DELETE) {
    return false;
  }

  if (plan->returningLists != NIL) {
    return false;
  }

  ForeignScan *fscan = pljs_fdw_find_modifytable_subplan(plan, resultRelation);

  if (fscan == NULL) {
    return false;
  }

  // Nothing left to recheck locally -- the whole WHERE clause is already
  // fully pushed down (see GetForeignPlan/"Qual pushdown" above).
  if (fscan->scan.plan.qual != NIL) {
    return false;
  }

  RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
  Relation rel = table_open(rte->relid, NoLock);

  bool has_row_trigger =
      rel->trigdesc &&
      ((operation == CMD_UPDATE && (rel->trigdesc->trig_update_before_row ||
                                    rel->trigdesc->trig_update_after_row)) ||
       (operation == CMD_DELETE && (rel->trigdesc->trig_delete_before_row ||
                                    rel->trigdesc->trig_delete_after_row)));

  if (has_row_trigger) {
    table_close(rel, NoLock);
    return false;
  }

  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(rte->relid, &ctx, &instance);

  const char *method_name =
      (operation == CMD_UPDATE) ? "updateWhere" : "deleteWhere";
  JSValue method_fn = JS_GetPropertyStr(ctx, instance, method_name);
  bool has_method = JS_IsFunction(ctx, method_fn);
  JS_FreeValue(ctx, method_fn);

  if (!has_method) {
    table_close(rel, NoLock);
    return false;
  }

  List *set_values = NIL;

  if (operation == CMD_UPDATE) {
    List *processed_tlist = NIL;
    List *update_colnos = NIL;

    get_translated_update_targetlist(root, resultRelation, &processed_tlist,
                                     &update_colnos);

    ListCell *lc1;
    ListCell *lc2;

    forboth(lc1, processed_tlist, lc2, update_colnos) {
      TargetEntry *tle = lfirst_node(TargetEntry, lc1);
      AttrNumber attno = lfirst_int(lc2);
      Node *expr = strip_implicit_coercions((Node *)tle->expr);

      // Only a plain constant assignment is direct-modify-safe here,
      // matching the same "simple values only" scoping as qual/sort
      // pushdown -- an expression referencing another column, a
      // subquery, etc. falls back to the per-row path instead.
      if (!IsA(expr, Const) || attno < 1) {
        table_close(rel, NoLock);
        return false;
      }

      pljs_fdw_qual *sv = (pljs_fdw_qual *)palloc(sizeof(pljs_fdw_qual));

      sv->attnum = attno;
      sv->opname = "=";
      sv->value = (Const *)expr;
      sv->rinfo = NULL;

      set_values = lappend(set_values, sv);
    }
  }

  // fdw_private[0] is already the pushable-quals encoding GetForeignPlan
  // produced for this scan (confirmed above to cover the whole WHERE
  // clause); reused as-is rather than re-derived from baserestrictinfo.
  List *quals_private = (List *)linitial(fscan->fdw_private);
  List *set_values_private = pljs_fdw_quals_to_plan_private(set_values);

  fscan->fdw_private =
      list_make4(quals_private, makeString(pstrdup(method_name)),
                set_values_private, makeInteger((int)plan->canSetTag));
  fscan->operation = operation;
  fscan->resultRelation = resultRelation;

  table_close(rel, NoLock);

  return true;
}

static void pljs_fdw_begin_direct_modify(ForeignScanState *node, int eflags) {
  if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
    return;
  }

  ForeignScan *fscan = (ForeignScan *)node->ss.ps.plan;

  JSContext *ctx;
  JSValue instance;
  pljs_fdw_get_or_create_instance(RelationGetRelid(node->ss.ss_currentRelation),
                                  &ctx, &instance);

  List *fdw_private = fscan->fdw_private;

  pljs_fdw_direct_modify_state *dmstate = (pljs_fdw_direct_modify_state *)palloc(
      sizeof(pljs_fdw_direct_modify_state));

  dmstate->ctx = ctx;
  dmstate->instance = instance;
  dmstate->operation = fscan->operation;
  dmstate->quals = pljs_fdw_quals_from_plan_private((List *)linitial(fdw_private));
  dmstate->set_values =
      pljs_fdw_quals_from_plan_private((List *)lthird(fdw_private));
  dmstate->can_set_tag = (bool)intVal(lfourth(fdw_private));
  dmstate->executed = false;

  node->fdw_state = dmstate;
}

static TupleTableSlot *pljs_fdw_iterate_direct_modify(ForeignScanState *node) {
  TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
  pljs_fdw_direct_modify_state *dmstate =
      (pljs_fdw_direct_modify_state *)node->fdw_state;

  ExecClearTuple(slot);

  if (dmstate == NULL || dmstate->executed) {
    return slot;
  }

  dmstate->executed = true;

  JSContext *ctx = dmstate->ctx;
  TupleDesc tupdesc = node->ss.ss_currentRelation->rd_att;
  JSValue quals_js = pljs_fdw_quals_to_jsvalue(ctx, dmstate->quals, tupdesc);
  const char *method_name =
      (dmstate->operation == CMD_UPDATE) ? "updateWhere" : "deleteWhere";
  JSValue method_fn = JS_GetPropertyStr(ctx, dmstate->instance, method_name);
  JSValue result;

  if (dmstate->operation == CMD_UPDATE) {
    JSValue values_js =
        pljs_fdw_set_values_to_jsvalue(ctx, dmstate->set_values, tupdesc);
    JSValueConst args[] = {quals_js, values_js};

    result = JS_Call(ctx, method_fn, dmstate->instance, 2, args);

    JS_FreeValue(ctx, values_js);
  } else {
    JSValueConst args[] = {quals_js};

    result = JS_Call(ctx, method_fn, dmstate->instance, 1, args);
  }

  JS_FreeValue(ctx, method_fn);
  JS_FreeValue(ctx, quals_js);

  if (JS_IsException(result)) {
    char *error_message = pljs_dump_error(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: %s() failed", method_name),
                    errdetail("%s", error_message)));
  }

  double count = 0;
  JS_ToFloat64(ctx, &count, result);
  JS_FreeValue(ctx, result);

  if (dmstate->can_set_tag) {
    node->ss.ps.state->es_processed += (uint64)count;
  }

  return slot;
}

/**
 * @brief No-op (ctx/instance are cache-owned; dmstate is just palloc'd).
 *
 * Registered anyway: unlike EndForeignModify/EndForeignInsert (genuinely
 * optional elsewhere), core Postgres's planner requires
 * FdwRoutine->EndDirectModify to be non-NULL as a precondition just to
 * attempt PlanDirectModify at all (see createplan.c's direct_modify
 * eligibility check) -- leaving it unset silently disables direct modify
 * entirely, regardless of what PlanDirectModify would have returned.
 */
static void pljs_fdw_end_direct_modify(ForeignScanState *node) {
}

static void pljs_fdw_explain_direct_modify(ForeignScanState *node,
                                           struct ExplainState *es) {
  if (!es->verbose) {
    return;
  }

  ForeignScan *fscan = (ForeignScan *)node->ss.ps.plan;
  const char *op = (fscan->operation == CMD_UPDATE)
                       ? "module-backed direct update"
                       : "module-backed direct delete";

  ExplainPropertyText("pljs FDW", op, es);
}

/*
 * ---------- TRUNCATE ----------
 *
 * A class opts in with:
 *
 *   truncate({ cascade, restartSequences }) { ... }
 *
 * `TRUNCATE t1, t2, ...` groups the given relations by server before
 * calling this (core Postgres's truncate_check_rel/ExecuteTruncate does
 * that grouping, in tablecmds.c) -- rels here may be several pljs_fdw
 * tables at once, each independently instantiated and each requiring its
 * own truncate() method (they aren't required to share a module). Unlike
 * INSERT/UPDATE/DELETE, TRUNCATE has no FdwRoutine-level "is this
 * supported" query analogous to IsForeignRelUpdatable -- core Postgres
 * only checks that ExecForeignTruncate itself is registered (see
 * truncate_check_rel), so a per-table capability check happens here
 * instead, same as insert()/update()/delete() already do.
 */

static void pljs_fdw_exec_foreign_truncate(List *rels, DropBehavior behavior,
                                           bool restart_seqs) {
  ListCell *lc;

  foreach (lc, rels) {
    Relation rel = (Relation)lfirst(lc);

    JSContext *ctx;
    JSValue instance;
    pljs_fdw_get_or_create_instance(RelationGetRelid(rel), &ctx, &instance);

    JSValue truncate_fn = JS_GetPropertyStr(ctx, instance, "truncate");

    if (!JS_IsFunction(ctx, truncate_fn)) {
      JS_FreeValue(ctx, truncate_fn);
      ereport(ERROR, (errmsg("pljs_fdw: foreign table \"%s\" does not "
                             "support TRUNCATE (no truncate() method)",
                             RelationGetRelationName(rel))));
    }

    JSValue options = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, options, "cascade",
                      JS_NewBool(ctx, behavior == DROP_CASCADE));
    JS_SetPropertyStr(ctx, options, "restartSequences",
                      JS_NewBool(ctx, restart_seqs));

    JSValueConst args[] = {options};
    JSValue result = JS_Call(ctx, truncate_fn, instance, 1, args);

    JS_FreeValue(ctx, truncate_fn);
    JS_FreeValue(ctx, options);

    if (JS_IsException(result)) {
      char *error_message = pljs_dump_error(ctx);
      ereport(ERROR,
              (errmsg("pljs_fdw: truncate() failed for foreign table \"%s\"",
                      RelationGetRelationName(rel)),
               errdetail("%s", error_message)));
    }

    JS_FreeValue(ctx, result);
  }
}

static void pljs_fdw_explain_foreign_scan(ForeignScanState *node,
                                          struct ExplainState *es) {
  ExplainPropertyText("pljs FDW", "module-backed foreign scan", es);
}

/*
 * ---------- Validator ----------
 *
 * The only option pljs_fdw understands at the server/table level is
 * "module" (see pljs_fdw_get_option). Rejecting anything else there
 * catches typos at DDL time (`CREATE SERVER ... OPTIONS (modle 'x')`)
 * rather than surfacing as a confusing "module option not set" error much
 * later, at first scan.
 *
 * User mapping options are deliberately NOT restricted to a fixed
 * allowlist: they're merged into the FDW instance's constructor options
 * as-is (see pljs_fdw_build_options), typically per-user credentials whose
 * names are entirely up to each FDW module's own author -- there's no
 * fixed set for the validator to check.
 *
 * Column-level options (`ALTER FOREIGN TABLE ... ALTER COLUMN col OPTIONS
 * (...)`, catalog AttributeRelationId) allow "column_name" -- see the
 * "Column mapping" section -- also required to be a non-empty string.
 *
 * pljs_fdw takes no options at the FDW level, so those are still rejected
 * outright.
 */

static const char *const pljs_fdw_recognized_options[] = {"module"};

static bool pljs_fdw_is_recognized_option(const char *name) {
  for (size_t i = 0; i < lengthof(pljs_fdw_recognized_options); i++) {
    if (strcmp(name, pljs_fdw_recognized_options[i]) == 0) {
      return true;
    }
  }

  return false;
}

PGDLLEXPORT Datum pljs_fdw_validator(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pljs_fdw_validator);

Datum pljs_fdw_validator(PG_FUNCTION_ARGS) {
  List *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
  Oid catalog = PG_GETARG_OID(1);
  bool module_allowed =
      (catalog == ForeignServerRelationId || catalog == ForeignTableRelationId);
  bool column_name_allowed = (catalog == AttributeRelationId);
  ListCell *lc;

  foreach (lc, options_list) {
    DefElem *def = lfirst_node(DefElem, lc);

    if ((module_allowed && pljs_fdw_is_recognized_option(def->defname)) ||
        (column_name_allowed && strcmp(def->defname, "column_name") == 0)) {
      char *value = defGetString(def);

      if (value == NULL || value[0] == '\0') {
        ereport(ERROR, (errmsg("pljs_fdw: \"%s\" option must be a non-empty "
                               "string",
                               def->defname)));
      }

      continue;
    }

    if (module_allowed) {
      ereport(ERROR, (errmsg("pljs_fdw: invalid option \"%s\"", def->defname),
                      errhint("Valid options here: module.")));
    } else if (column_name_allowed) {
      ereport(ERROR, (errmsg("pljs_fdw: invalid option \"%s\"", def->defname),
                      errhint("Valid options here: column_name.")));
    } else if (catalog == UserMappingRelationId) {
      // Unrestricted: whatever credential names an FDW module's author
      // chose (see the "Validator" section comment above).
      continue;
    } else if (catalog == ForeignDataWrapperRelationId) {
      ereport(ERROR, (errmsg("pljs_fdw: invalid option \"%s\"", def->defname),
                      errhint("pljs_fdw takes no options at the foreign-data-"
                             "wrapper level.")));
    } else {
      ereport(ERROR, (errmsg("pljs_fdw: invalid option \"%s\"", def->defname)));
    }
  }

  PG_RETURN_VOID();
}

/*
 * ---------- IMPORT FOREIGN SCHEMA ----------
 *
 * `IMPORT FOREIGN SCHEMA remote_schema FROM SERVER srv INTO local_schema
 * [LIMIT TO (...) | EXCEPT (...)] [OPTIONS (...)]` has no natural remote
 * catalog to query here (unlike postgres_fdw), so `remote_schema` is
 * treated as a pljs.modules path to a schema-*describing* module -- a
 * different shape than the per-table class everything else in this file
 * is built around, since one IMPORT can produce many tables at once:
 *
 *   module.exports = function(options) {
 *     return {
 *       orders:    { columns: { id: 'integer', total: 'numeric' },
 *                    options: { module: 'orders_impl' } },
 *       customers: { columns: { id: 'integer',
 *                               // { type, name } declares column_name too
 *                               // (see "Column mapping") -- a plain
 *                               // string means no mapping, as above.
 *                               name: { type: 'text', name: 'cust_name' } },
 *                    options: { module: 'customers_impl' } },
 *     };
 *   };
 *
 * `options` is IMPORT's own OPTIONS (...) clause, if given. Each column's
 * type is emitted verbatim as a Postgres type name -- not validated here,
 * since the generated CREATE FOREIGN TABLE statement already gets parsed
 * (and any bad type name rejected) when Postgres executes it. This whole
 * callback only produces DDL text for the caller to run; unlike
 * everything else in this file, a mistake here surfaces as an ordinary
 * parse/DDL error, not a silently wrong query result.
 */

/**
 * @brief Whether @p table_name should be imported, per IMPORT's optional
 * `LIMIT TO (...)` / `EXCEPT (...)` clause.
 */
static bool pljs_fdw_import_table_included(ImportForeignSchemaStmt *stmt,
                                           const char *table_name) {
  if (stmt->list_type == FDW_IMPORT_SCHEMA_ALL) {
    return true;
  }

  bool found = false;
  ListCell *lc;

  foreach (lc, stmt->table_list) {
    RangeVar *rv = lfirst_node(RangeVar, lc);

    if (strcmp(rv->relname, table_name) == 0) {
      found = true;
      break;
    }
  }

  return (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO) ? found : !found;
}

/**
 * @brief Builds one `CREATE FOREIGN TABLE ...;` string for @p table_name
 * from its schema-module entry (`{ columns, options }`), appending it to
 * @p commands.
 */
static void pljs_fdw_append_import_command(List **commands, JSContext *ctx,
                                           ImportForeignSchemaStmt *stmt,
                                           const char *table_name,
                                           JSValue table_def) {
  if (!JS_IsObject(table_def)) {
    ereport(ERROR, (errmsg("pljs_fdw: table \"%s\" in schema module \"%s\" "
                           "must be an object",
                           table_name, stmt->remote_schema)));
  }

  JSValue columns = JS_GetPropertyStr(ctx, table_def, "columns");
  JSPropertyEnum *col_tab;
  uint32_t col_len;

  if (!JS_IsObject(columns) ||
      JS_GetOwnPropertyNames(ctx, &col_tab, &col_len, columns,
                             JS_GPN_STRING_MASK) < 0 ||
      col_len == 0) {
    JS_FreeValue(ctx, columns);
    ereport(ERROR, (errmsg("pljs_fdw: table \"%s\" in schema module \"%s\" "
                           "has no \"columns\"",
                           table_name, stmt->remote_schema)));
  }

  StringInfoData buf;
  initStringInfo(&buf);

  appendStringInfo(&buf, "CREATE FOREIGN TABLE %s.%s (",
                   quote_identifier(stmt->local_schema),
                   quote_identifier(table_name));

  for (uint32_t i = 0; i < col_len; i++) {
    const char *col_name = JS_AtomToCString(ctx, col_tab[i].atom);
    JSValue col_def = JS_GetPropertyStr(ctx, columns, col_name);

    // A column's value is either a plain type string (no mapping), or
    // { type, name } to also declare a "column_name" option -- see the
    // "Column mapping" section.
    JSValue col_type_val =
        JS_IsObject(col_def) ? JS_GetPropertyStr(ctx, col_def, "type")
                             : JS_DupValue(ctx, col_def);
    const char *col_type = JS_ToCString(ctx, col_type_val);

    if (i > 0) {
      appendStringInfoString(&buf, ", ");
    }

    // col_type is emitted verbatim -- not our job to validate a Postgres
    // type name, the CREATE FOREIGN TABLE statement itself will.
    appendStringInfo(&buf, "%s %s", quote_identifier(col_name), col_type);

    if (JS_IsObject(col_def)) {
      JSValue name_val = JS_GetPropertyStr(ctx, col_def, "name");

      if (JS_IsString(name_val)) {
        const char *remote_name = JS_ToCString(ctx, name_val);

        appendStringInfo(&buf, " OPTIONS (column_name %s)",
                         quote_literal_cstr(remote_name));

        JS_FreeCString(ctx, remote_name);
      }

      JS_FreeValue(ctx, name_val);
    }

    JS_FreeCString(ctx, col_type);
    JS_FreeValue(ctx, col_type_val);
    JS_FreeValue(ctx, col_def);
    JS_FreeCString(ctx, col_name);
    JS_FreeAtom(ctx, col_tab[i].atom);
  }

  js_free(ctx, col_tab);
  JS_FreeValue(ctx, columns);

  appendStringInfo(&buf, ") SERVER %s", quote_identifier(stmt->server_name));

  JSValue table_options = JS_GetPropertyStr(ctx, table_def, "options");
  JSPropertyEnum *opt_tab;
  uint32_t opt_len;

  if (JS_IsObject(table_options) &&
      JS_GetOwnPropertyNames(ctx, &opt_tab, &opt_len, table_options,
                             JS_GPN_STRING_MASK) == 0 &&
      opt_len > 0) {
    appendStringInfoString(&buf, " OPTIONS (");

    for (uint32_t i = 0; i < opt_len; i++) {
      const char *opt_name = JS_AtomToCString(ctx, opt_tab[i].atom);
      JSValue opt_val = JS_GetPropertyStr(ctx, table_options, opt_name);
      const char *opt_str = JS_ToCString(ctx, opt_val);

      if (i > 0) {
        appendStringInfoString(&buf, ", ");
      }

      appendStringInfo(&buf, "%s %s", quote_identifier(opt_name),
                       quote_literal_cstr(opt_str));

      JS_FreeCString(ctx, opt_str);
      JS_FreeValue(ctx, opt_val);
      JS_FreeCString(ctx, opt_name);
      JS_FreeAtom(ctx, opt_tab[i].atom);
    }

    js_free(ctx, opt_tab);
    appendStringInfoChar(&buf, ')');
  }

  JS_FreeValue(ctx, table_options);
  appendStringInfoChar(&buf, ';');

  *commands = lappend(*commands, buf.data);
}

static List *pljs_fdw_import_foreign_schema(ImportForeignSchemaStmt *stmt,
                                             Oid serverOid) {
  List *commands = NIL;

  JSContext *ctx = JS_NewContext(pljs_fdw_rt);
  pljs_setup_namespace(ctx);
  pljs_fdw_extend_namespace(ctx);

  JSValue module_fn = pljs_module_require(ctx, stmt->remote_schema);

  if (JS_IsException(module_fn)) {
    char *error_message = pljs_dump_error(ctx);
    JS_FreeContext(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: unable to load schema module \"%s\"",
                           stmt->remote_schema),
                    errdetail("%s", error_message)));
  }

  if (!JS_IsFunction(ctx, module_fn)) {
    JS_FreeValue(ctx, module_fn);
    JS_FreeContext(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: schema module \"%s\" must export a "
                           "function",
                           stmt->remote_schema)));
  }

  JSValue options = JS_NewObject(ctx);
  ListCell *lc;

  foreach (lc, stmt->options) {
    DefElem *def = lfirst_node(DefElem, lc);
    JS_SetPropertyStr(ctx, options, def->defname,
                      JS_NewString(ctx, defGetString(def)));
  }

  JSValueConst call_args[] = {options};
  JSValue schema = JS_Call(ctx, module_fn, JS_UNDEFINED, 1, call_args);

  JS_FreeValue(ctx, module_fn);
  JS_FreeValue(ctx, options);

  if (JS_IsException(schema)) {
    char *error_message = pljs_dump_error(ctx);
    JS_FreeContext(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: schema module \"%s\" failed",
                           stmt->remote_schema),
                    errdetail("%s", error_message)));
  }

  JSPropertyEnum *tab;
  uint32_t len;

  if (!JS_IsObject(schema) ||
      JS_GetOwnPropertyNames(ctx, &tab, &len, schema, JS_GPN_STRING_MASK) <
          0) {
    JS_FreeValue(ctx, schema);
    JS_FreeContext(ctx);
    ereport(ERROR, (errmsg("pljs_fdw: schema module \"%s\" must return an "
                           "object of {tableName: {columns, options}}",
                           stmt->remote_schema)));
  }

  for (uint32_t i = 0; i < len; i++) {
    const char *table_name = JS_AtomToCString(ctx, tab[i].atom);

    if (pljs_fdw_import_table_included(stmt, table_name)) {
      JSValue table_def = JS_GetPropertyStr(ctx, schema, table_name);

      pljs_fdw_append_import_command(&commands, ctx, stmt, table_name,
                                     table_def);

      JS_FreeValue(ctx, table_def);
    }

    JS_FreeCString(ctx, table_name);
    JS_FreeAtom(ctx, tab[i].atom);
  }

  js_free(ctx, tab);
  JS_FreeValue(ctx, schema);
  JS_FreeContext(ctx);

  return commands;
}

/**
 * @brief Handler for the PLJS foreign data wrapper.
 *
 * Returns the #FdwRoutine callback table PostgreSQL uses to plan and
 * execute scans against foreign tables backed by this FDW.
 */
PGDLLEXPORT Datum pljs_fdw_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pljs_fdw_handler);

Datum pljs_fdw_handler(PG_FUNCTION_ARGS) {
  pljs_fdw_bind_pljs_functions();

  FdwRoutine *routine = makeNode(FdwRoutine);

  routine->GetForeignRelSize = pljs_fdw_get_foreign_rel_size;
  routine->GetForeignPaths = pljs_fdw_get_foreign_paths;
  routine->GetForeignPlan = pljs_fdw_get_foreign_plan;
  routine->BeginForeignScan = pljs_fdw_begin_foreign_scan;
  routine->IterateForeignScan = pljs_fdw_iterate_foreign_scan;
  routine->ReScanForeignScan = pljs_fdw_rescan_foreign_scan;
  routine->EndForeignScan = pljs_fdw_end_foreign_scan;
  routine->ExplainForeignScan = pljs_fdw_explain_foreign_scan;

  routine->IsForeignRelUpdatable = pljs_fdw_is_foreign_rel_updatable;
  routine->AddForeignUpdateTargets = pljs_fdw_add_foreign_update_targets;
  routine->BeginForeignModify = pljs_fdw_begin_foreign_modify;
  routine->ExecForeignInsert = pljs_fdw_exec_foreign_insert;
  routine->ExecForeignUpdate = pljs_fdw_exec_foreign_update;
  routine->ExecForeignDelete = pljs_fdw_exec_foreign_delete;
  routine->BeginForeignInsert = pljs_fdw_begin_foreign_insert;
  routine->GetForeignModifyBatchSize = pljs_fdw_get_foreign_modify_batch_size;
  routine->ExecForeignBatchInsert = pljs_fdw_exec_foreign_batch_insert;
  routine->ExplainForeignModify = pljs_fdw_explain_foreign_modify;

  routine->PlanDirectModify = pljs_fdw_plan_direct_modify;
  routine->BeginDirectModify = pljs_fdw_begin_direct_modify;
  routine->IterateDirectModify = pljs_fdw_iterate_direct_modify;
  routine->EndDirectModify = pljs_fdw_end_direct_modify;
  routine->ExplainDirectModify = pljs_fdw_explain_direct_modify;

  routine->ExecForeignTruncate = pljs_fdw_exec_foreign_truncate;

  routine->ImportForeignSchema = pljs_fdw_import_foreign_schema;

  PG_RETURN_POINTER(routine);
}
