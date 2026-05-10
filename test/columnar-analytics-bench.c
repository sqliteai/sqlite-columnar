/*
** Analytics benchmark inspired by DuckDB's H2O.ai group-by benchmark.
**
** It compares:
**   1. SQLite row-store table scans.
**   2. The experimental columnar virtual table scans.
**   3. Specialized columnar_sum/columnar_avg/columnar_count functions.
**   4. Specialized grouped columnar table-valued functions.
**   5. Specialized range-filtered functions backed by chunk zone maps.
**
** The schema is a wide fact table:
**   ts/id1..id6 dimensions, v1..v3 measures, cold0..cold3 unused columns.
**
** Build from the repository root:
**
**   make build/columnar-analytics-bench
**
** Run:
**
**   build/columnar-analytics-bench ./columnar 10000000
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static const char *azName[] = {
  "ts",
  "id1", "id2", "id3", "id4", "id5", "id6",
  "v1", "v2", "v3",
  "cold0", "cold1", "coldtxt0", "coldtxt1"
};

static const char *azType[] = {
  "INTEGER",
  "INTEGER", "INTEGER", "INTEGER", "INTEGER", "INTEGER", "INTEGER",
  "INTEGER", "INTEGER", "REAL",
  "INTEGER", "INTEGER", "TEXT", "TEXT"
};

static const char *azExpr[] = {
  "i",
  "i%100",
  "i%1000",
  "i%10000",
  "i%10",
  "i%100000",
  "i%7",
  "i%100",
  "(i*7)%1000",
  "(i%1000)*0.1",
  "(i*13)%1000000",
  "(i*17)%1000000"
};

static double now_ms(void){
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (double)tv.tv_sec*1000.0 + (double)tv.tv_usec/1000.0;
}

static void fatal(sqlite3 *db, const char *zMsg, int rc){
  fprintf(stderr, "%s: %s (%d)\n", zMsg, db ? sqlite3_errmsg(db) : "", rc);
  exit(1);
}

static void execsql(sqlite3 *db, const char *zSql){
  char *zErr = 0;
  int rc = sqlite3_exec(db, zSql, 0, 0, &zErr);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "SQL error: %s\nSQL: %s\n", zErr ? zErr : sqlite3_errmsg(db), zSql);
    sqlite3_free(zErr);
    exit(1);
  }
}

static void execf(sqlite3 *db, const char *zFmt, sqlite3_int64 nRow){
  char *zSql = sqlite3_mprintf(zFmt, nRow);
  if( zSql==0 ) fatal(db, "sqlite3_mprintf", SQLITE_NOMEM);
  execsql(db, zSql);
  sqlite3_free(zSql);
}

static void exec_insert_one(sqlite3 *db, const char *zTab, sqlite3_int64 i){
  char *zSql = sqlite3_mprintf(
      "INSERT INTO %s(rowid,ts,id1,id2,id3,id4,id5,id6,v1,v2,v3,"
      "cold0,cold1,coldtxt0,coldtxt1) VALUES("
      "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%.17g,"
      "%lld,%lld,'incremental-a','incremental-b')",
      zTab,
      (long long)i,
      (long long)i,
      (long long)(i%100),
      (long long)(i%1000),
      (long long)(i%10000),
      (long long)(i%10),
      (long long)(i%100000),
      (long long)(i%7),
      (long long)(i%100),
      (long long)((i*7)%1000),
      (double)(i%1000)*0.1,
      (long long)((i*13)%1000000),
      (long long)((i*17)%1000000));
  if( zSql==0 ) fatal(db, "sqlite3_mprintf", SQLITE_NOMEM);
  execsql(db, zSql);
  sqlite3_free(zSql);
}

static sqlite3_int64 db_bytes(sqlite3 *db){
  sqlite3_stmt *pStmt = 0;
  sqlite3_int64 nPage = 0;
  sqlite3_int64 nPageSize = 0;
  if( sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &pStmt, 0)==SQLITE_OK
   && sqlite3_step(pStmt)==SQLITE_ROW
  ){
    nPage = sqlite3_column_int64(pStmt, 0);
  }
  sqlite3_finalize(pStmt);
  pStmt = 0;
  if( sqlite3_prepare_v2(db, "PRAGMA page_size", -1, &pStmt, 0)==SQLITE_OK
   && sqlite3_step(pStmt)==SQLITE_ROW
  ){
    nPageSize = sqlite3_column_int64(pStmt, 0);
  }
  sqlite3_finalize(pStmt);
  return nPage * nPageSize;
}

static sqlite3_int64 int64_query(sqlite3 *db, const char *zSql){
  sqlite3_stmt *pStmt = 0;
  sqlite3_int64 v = 0;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ) fatal(db, "prepare int64 query", rc);
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    v = sqlite3_column_int64(pStmt, 0);
    rc = sqlite3_step(pStmt);
  }
  if( rc!=SQLITE_DONE ) fatal(db, "int64 query", rc);
  sqlite3_finalize(pStmt);
  return v;
}

static void setup_pragmas(sqlite3 *db){
  execsql(db,
      "PRAGMA journal_mode=OFF;"
      "PRAGMA synchronous=OFF;"
      "PRAGMA temp_store=MEMORY;"
      "PRAGMA locking_mode=EXCLUSIVE;"
      "PRAGMA cache_size=-300000");
}

static void open_db(const char *zPath, sqlite3 **ppDb){
  int rc = sqlite3_open(zPath, ppDb);
  if( rc!=SQLITE_OK ) fatal(*ppDb, "open", rc);
}

static int is_integer_arg(const char *z){
  if( z==0 || z[0]==0 ) return 0;
  while( z[0]>='0' && z[0]<='9' ) z++;
  return z[0]==0;
}

static void load_columnar(sqlite3 *db, const char *zExt){
  char *zErr = 0;
  int rc;
  if( zExt==0 || zExt[0]==0 ) return;
  sqlite3_enable_load_extension(db, 1);
  rc = sqlite3_load_extension(db, zExt, 0, &zErr);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "load extension: %s\n", zErr ? zErr : sqlite3_errmsg(db));
    sqlite3_free(zErr);
    exit(1);
  }
}

static char *schema_cols(void){
  sqlite3_str *p = sqlite3_str_new(0);
  int i;
  for(i=0; i<(int)(sizeof(azName)/sizeof(azName[0])); i++){
    if( i ) sqlite3_str_appendall(p, ",");
    sqlite3_str_appendf(p, "%s %s", azName[i], azType[i]);
  }
  return sqlite3_str_finish(p);
}

static char *payload_literal(int nByte, int iText){
  char *zPayload;
  int i;
  if( nByte<1 ) nByte = 1;
  zPayload = sqlite3_malloc64((sqlite3_uint64)nByte + 1);
  if( zPayload==0 ) return 0;
  for(i=0; i<nByte; i++){
    zPayload[i] = (char)('a' + ((i+iText*7)%26));
  }
  zPayload[nByte] = 0;
  return zPayload;
}

static char *expr_for_col(int iCol, int nPayload){
  if( iCol<(int)(sizeof(azExpr)/sizeof(azExpr[0])) ){
    return sqlite3_mprintf("%s", azExpr[iCol]);
  }else{
    char *zPayload = payload_literal(nPayload, iCol);
    char *zExpr;
    if( zPayload==0 ) return 0;
    zExpr = sqlite3_mprintf("%Q", zPayload);
    sqlite3_free(zPayload);
    return zExpr;
  }
}

static char *expr_list(int nPayload){
  sqlite3_str *p = sqlite3_str_new(0);
  int i;
  for(i=0; i<(int)(sizeof(azName)/sizeof(azName[0])); i++){
    char *zExpr = expr_for_col(i, nPayload);
    if( zExpr==0 ){
      sqlite3_str_reset(p);
      return 0;
    }
    if( i ) sqlite3_str_appendall(p, ",");
    sqlite3_str_appendall(p, zExpr);
    sqlite3_free(zExpr);
  }
  return sqlite3_str_finish(p);
}

static void populate_row(sqlite3 *db, sqlite3_int64 nRow, int nPayload){
  char *zCols = schema_cols();
  char *zExpr = expr_list(nPayload);
  char *zSql;
  if( zCols==0 || zExpr==0 ) fatal(db, "oom", SQLITE_NOMEM);
  zSql = sqlite3_mprintf("CREATE TABLE r(%s)", zCols);
  if( zSql==0 ) fatal(db, "oom", SQLITE_NOMEM);
  execsql(db, zSql);
  sqlite3_free(zSql);
  zSql = sqlite3_mprintf(
      "WITH RECURSIVE s(i) AS ("
      "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%lld"
      ") INSERT INTO r SELECT %s FROM s",
      nRow, zExpr);
  if( zSql==0 ) fatal(db, "oom", SQLITE_NOMEM);
  execsql(db, zSql);
  sqlite3_free(zSql);
  sqlite3_free(zCols);
  sqlite3_free(zExpr);
}

static void populate_columnar(sqlite3 *db, sqlite3_int64 nRow, int nPayload){
  char *zCols = schema_cols();
  char *zSql;
  int i;
  if( zCols==0 ) fatal(db, "oom", SQLITE_NOMEM);
  zSql = sqlite3_mprintf("CREATE VIRTUAL TABLE c USING columnar(%s)", zCols);
  if( zSql==0 ) fatal(db, "oom", SQLITE_NOMEM);
  execsql(db, zSql);
  sqlite3_free(zSql);
  sqlite3_free(zCols);

  execsql(db, "BEGIN");
  execf(db,
      "WITH RECURSIVE s(i) AS ("
      "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%lld"
      ") INSERT INTO c__columnar_rowid(rid) SELECT i FROM s",
      nRow);
  for(i=0; i<(int)(sizeof(azName)/sizeof(azName[0])); i++){
    char *zExpr = expr_for_col(i, nPayload);
    if( zExpr==0 ) fatal(db, "oom", SQLITE_NOMEM);
    zSql = sqlite3_mprintf(
        "WITH RECURSIVE s(i) AS ("
        "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%lld"
        ") INSERT INTO c__columnar_c%d(rid,v) SELECT i, %s FROM s",
        nRow, i, zExpr);
    sqlite3_free(zExpr);
    if( zSql==0 ) fatal(db, "oom", SQLITE_NOMEM);
    execsql(db, zSql);
    sqlite3_free(zSql);
  }
  execsql(db, "COMMIT");
}

static double run_query(sqlite3 *db, const char *zSql, double *pChecksum, int *pnRow){
  sqlite3_stmt *pStmt = 0;
  double t0;
  double r = 0.0;
  int n = 0;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ) fatal(db, "prepare query", rc);
  t0 = now_ms();
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    int i;
    int nCol = sqlite3_column_count(pStmt);
    n++;
    for(i=0; i<nCol; i++){
      r += sqlite3_column_double(pStmt, i) * (double)(i+1);
    }
  }
  if( rc!=SQLITE_DONE ) fatal(db, "query", rc);
  sqlite3_finalize(pStmt);
  *pChecksum = r;
  *pnRow = n;
  return now_ms() - t0;
}

static void compare(
  sqlite3 *dbRow,
  sqlite3 *dbCol,
  const char *zName,
  const char *zRowSql,
  const char *zColSql
){
  double rowChk = 0.0, colChk = 0.0;
  double rowMs, colMs;
  int rowN = 0, colN = 0;
  rowMs = run_query(dbRow, zRowSql, &rowChk, &rowN);
  colMs = run_query(dbCol, zColSql, &colChk, &colN);
  printf("%s row_ms=%.3f columnar_ms=%.3f speedup=%.2fx rows=%d/%d checksum=%.3f/%.3f\n",
      zName, rowMs, colMs, colMs>0.0 ? rowMs/colMs : 0.0,
      rowN, colN, rowChk, colChk);
  fflush(stdout);
}

static void compare_dynamic(
  sqlite3 *dbRow,
  sqlite3 *dbCol,
  const char *zName,
  char *zRowSql,
  char *zColSql
){
  if( zRowSql==0 || zColSql==0 ) fatal(dbCol, "sqlite3_mprintf", SQLITE_NOMEM);
  compare(dbRow, dbCol, zName, zRowSql, zColSql);
  sqlite3_free(zRowSql);
  sqlite3_free(zColSql);
}

int main(int argc, char **argv){
  const char *zExt = "./columnar";
  int iArg = 1;
  sqlite3_int64 nRow;
  int nPayload;
  const char *zRowDb = "/tmp/sqlite-row-analytics.db";
  const char *zColDb = "/tmp/sqlite-columnar-analytics.db";
  sqlite3 *dbRow = 0;
  sqlite3 *dbCol = 0;
  double t0;
  double tAnalyze;
  sqlite3_int64 iRangeLo;
  sqlite3_int64 iRangeHi;
  sqlite3_int64 nTsChunk;
  sqlite3_int64 nTsSelectedChunk;
  sqlite3_int64 nTsFullChunk;
  char *zChunkSql;

  if( argc>1 && !is_integer_arg(argv[1]) ){
    zExt = argv[1];
    iArg++;
  }
  nRow = argc>iArg ? atoll(argv[iArg]) : 10000000;
  nPayload = argc>iArg+1 ? atoi(argv[iArg+1]) : 256;
  iRangeLo = nRow/100;
  iRangeHi = iRangeLo + nRow/100;
  if( iRangeLo<1 ) iRangeLo = 1;
  if( iRangeHi>nRow ) iRangeHi = nRow;

  unlink(zRowDb);
  unlink(zColDb);
  open_db(zRowDb, &dbRow);
  open_db(zColDb, &dbCol);
  load_columnar(dbCol, zExt);
  setup_pragmas(dbRow);
  setup_pragmas(dbCol);

  printf("rows=%lld payload_bytes_per_cold_text_col=%d cold_text_cols=2\n",
      (long long)nRow, nPayload);
  printf("populate=row\n");
  fflush(stdout);
  t0 = now_ms();
  populate_row(dbRow, nRow, nPayload);
  printf("populate_row_ms=%.3f\n", now_ms()-t0);
  printf("populate=columnar\n");
  fflush(stdout);
  t0 = now_ms();
  populate_columnar(dbCol, nRow, nPayload);
  printf("populate_columnar_ms=%.3f\n", now_ms()-t0);
  t0 = now_ms();
  execsql(dbCol, "SELECT columnar_analyze('c')");
  tAnalyze = now_ms()-t0;
  printf("columnar_analyze_ms=%.3f\n", tAnalyze);
  t0 = now_ms();
  execsql(dbCol, "SELECT columnar_analyze('c')");
  printf("columnar_analyze_noop_ms=%.3f\n", now_ms()-t0);
  printf("columnar_meta_after_analyze row_count=%lld chunk_count=%lld"
         " dirty_count=%lld stats_valid=%lld\n",
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='row_count'"),
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='chunk_count'"),
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='dirty_count'"),
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='stats_valid'"));
  printf("db_bytes row=%lld columnar=%lld\n",
      (long long)db_bytes(dbRow), (long long)db_bytes(dbCol));
  nTsChunk = int64_query(dbCol,
      "SELECT count(*) FROM c__columnar_chunk WHERE col=0");
  zChunkSql = sqlite3_mprintf(
      "SELECT count(*) FROM c__columnar_chunk"
      " WHERE col=0 AND min IS NOT NULL AND max>=%lld AND min<=%lld",
      (long long)iRangeLo, (long long)iRangeHi);
  if( zChunkSql==0 ) fatal(dbCol, "sqlite3_mprintf", SQLITE_NOMEM);
  nTsSelectedChunk = int64_query(dbCol, zChunkSql);
  sqlite3_free(zChunkSql);
  zChunkSql = sqlite3_mprintf(
      "SELECT count(*) FROM c__columnar_chunk"
      " WHERE col=0 AND nonnull_count=row_count"
      " AND min>=%lld AND max<=%lld",
      (long long)iRangeLo, (long long)iRangeHi);
  if( zChunkSql==0 ) fatal(dbCol, "sqlite3_mprintf", SQLITE_NOMEM);
  nTsFullChunk = int64_query(dbCol, zChunkSql);
  sqlite3_free(zChunkSql);
  printf("zone_map_ts_range lo=%lld hi=%lld selected_chunks=%lld full_cover_chunks=%lld total_chunks=%lld\n",
      (long long)iRangeLo, (long long)iRangeHi,
      (long long)nTsSelectedChunk, (long long)nTsFullChunk,
      (long long)nTsChunk);
  fflush(stdout);

  compare(dbRow, dbCol, "full_sum_v1",
      "SELECT sum(v1) FROM r",
      "SELECT columnar_sum('c','v1')");
  compare(dbRow, dbCol, "full_avg_v3",
      "SELECT avg(v3) FROM r",
      "SELECT columnar_avg('c','v3')");
  compare(dbRow, dbCol, "full_count_v1",
      "SELECT count(v1) FROM r",
      "SELECT columnar_count('c','v1')");
  compare(dbRow, dbCol, "h2o_q01_group_id1_sum_v1",
      "SELECT id1, sum(v1) FROM r GROUP BY id1 ORDER BY id1",
      "SELECT id1, sum(v1) FROM c GROUP BY id1 ORDER BY id1");
  compare(dbRow, dbCol, "specialized_group_sum_id1_v1",
      "SELECT id1, sum(v1) FROM r GROUP BY id1 ORDER BY id1",
      "SELECT k, \"sum\" FROM columnar_group_sum('c','id1','v1') ORDER BY k");
  compare(dbRow, dbCol, "specialized_group_avg_id1_v3",
      "SELECT id1, avg(v3) FROM r GROUP BY id1 ORDER BY id1",
      "SELECT k, \"avg\" FROM columnar_group_avg('c','id1','v3') ORDER BY k");
  compare(dbRow, dbCol, "specialized_group_count_id1",
      "SELECT id1, count(*) FROM r GROUP BY id1 ORDER BY id1",
      "SELECT k, \"count\" FROM columnar_group_count('c','id1') ORDER BY k");
  compare(dbRow, dbCol, "specialized_group_sum_avg_count_id1_v1",
      "SELECT id1, sum(v1), avg(v1), count(v1) FROM r GROUP BY id1 ORDER BY id1",
      "SELECT k, \"sum\", \"avg\", \"count\" "
      "FROM columnar_group_sum_avg_count('c','id1','v1') ORDER BY k");
  compare(dbRow, dbCol, "h2o_q03_group_id3_sum_avg",
      "SELECT id3, sum(v1), avg(v3) FROM r GROUP BY id3 ORDER BY id3",
      "SELECT id3, sum(v1), avg(v3) FROM c GROUP BY id3 ORDER BY id3");
  compare(dbRow, dbCol, "h2o_q07_group_id3_range",
      "SELECT id3, max(v1)-min(v2) FROM r GROUP BY id3 ORDER BY id3",
      "SELECT id3, max(v1)-min(v2) FROM c GROUP BY id3 ORDER BY id3");
  compare(dbRow, dbCol, "specialized_group_min_max_count_id3_v1",
      "SELECT id3, min(v1), max(v1), count(v1) FROM r GROUP BY id3 ORDER BY id3",
      "SELECT k, \"min\", \"max\", \"count\" "
      "FROM columnar_group_min_max_count('c','id3','v1') ORDER BY k");
  compare(dbRow, dbCol, "specialized_group_range_id3_v1_v2",
      "SELECT id3, max(v1)-min(v2), max(v1), min(v2), count(*) "
      "FROM r GROUP BY id3 ORDER BY id3",
      "SELECT k, \"range\", \"max\", \"min\", \"count\" "
      "FROM columnar_group_range('c','id3','v1','v2') ORDER BY k");
  compare(dbRow, dbCol, "filter_sum_id2",
      "SELECT sum(v1), avg(v3) FROM r WHERE id2 BETWEEN 10 AND 20",
      "SELECT sum(v1), avg(v3) FROM c WHERE id2 BETWEEN 10 AND 20");
  compare(dbRow, dbCol, "specialized_filter_sum_avg_count_id2",
      "SELECT sum(v1), avg(v3), count(v1) FROM r WHERE id2 BETWEEN 10 AND 20",
      "SELECT columnar_sum_where_range('c','v1','id2',10,20),"
      " columnar_avg_where_range('c','v3','id2',10,20),"
      " columnar_count_where_range('c','v1','id2',10,20)");
  compare_dynamic(dbRow, dbCol, "clustered_ts_filter_generic",
      sqlite3_mprintf(
        "SELECT sum(v1), avg(v3), count(v1) FROM r"
        " WHERE ts BETWEEN %lld AND %lld",
        (long long)iRangeLo, (long long)iRangeHi),
      sqlite3_mprintf(
        "SELECT sum(v1), avg(v3), count(v1) FROM c"
        " WHERE ts BETWEEN %lld AND %lld",
        (long long)iRangeLo, (long long)iRangeHi));
  compare_dynamic(dbRow, dbCol, "clustered_ts_filter_specialized",
      sqlite3_mprintf(
        "SELECT sum(v1), avg(v3), count(v1) FROM r"
        " WHERE ts BETWEEN %lld AND %lld",
        (long long)iRangeLo, (long long)iRangeHi),
      sqlite3_mprintf(
        "SELECT columnar_sum_where_range('c','v1','ts',%lld,%lld),"
        " columnar_avg_where_range('c','v3','ts',%lld,%lld),"
        " columnar_count_where_range('c','v1','ts',%lld,%lld)",
        (long long)iRangeLo, (long long)iRangeHi,
        (long long)iRangeLo, (long long)iRangeHi,
        (long long)iRangeLo, (long long)iRangeHi));
  compare_dynamic(dbRow, dbCol, "clustered_ts_group_sum_specialized",
      sqlite3_mprintf(
        "SELECT id1, sum(v1) FROM r WHERE ts BETWEEN %lld AND %lld"
        " GROUP BY id1 ORDER BY id1",
        (long long)iRangeLo, (long long)iRangeHi),
      sqlite3_mprintf(
        "SELECT k, \"sum\" FROM columnar_group_sum_where_range"
        "('c','id1','v1','ts',%lld,%lld) ORDER BY k",
        (long long)iRangeLo, (long long)iRangeHi));
  compare_dynamic(dbRow, dbCol, "clustered_ts_group_sum_avg_count_specialized",
      sqlite3_mprintf(
        "SELECT id1, sum(v1), avg(v1), count(v1) FROM r"
        " WHERE ts BETWEEN %lld AND %lld GROUP BY id1 ORDER BY id1",
        (long long)iRangeLo, (long long)iRangeHi),
      sqlite3_mprintf(
        "SELECT k, \"sum\", \"avg\", \"count\""
        " FROM columnar_group_sum_avg_count_where_range"
        "('c','id1','v1','ts',%lld,%lld) ORDER BY k",
        (long long)iRangeLo, (long long)iRangeHi));

  execsql(dbCol, "BEGIN");
  exec_insert_one(dbCol, "c", nRow+1);
  execsql(dbCol, "COMMIT");
  printf("incremental_dirty_entries_after_one_insert=%lld\n",
      (long long)int64_query(dbCol,
        "SELECT count(*) FROM c__columnar_dirty"));
  printf("incremental_meta_dirty_count_after_one_insert=%lld\n",
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='dirty_count'"));
  t0 = now_ms();
  execsql(dbCol, "SELECT columnar_analyze('c')");
  printf("columnar_analyze_one_insert_ms=%.3f\n", now_ms()-t0);
  printf("incremental_dirty_entries_after_analyze=%lld\n",
      (long long)int64_query(dbCol,
        "SELECT count(*) FROM c__columnar_dirty"));
  printf("incremental_meta_after_analyze row_count=%lld chunk_count=%lld"
         " dirty_count=%lld stats_valid=%lld\n",
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='row_count'"),
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='chunk_count'"),
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='dirty_count'"),
      (long long)int64_query(dbCol,
        "SELECT v FROM c__columnar_meta WHERE k='stats_valid'"));

  sqlite3_close(dbRow);
  sqlite3_close(dbCol);
  return 0;
}
