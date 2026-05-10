/*
** Benchmark row-store aggregate functions against the experimental
** columnar_* specialized functions on a single INTEGER column.
**
** Build from the repository root:
**
**   make build/columnar-agg-bench
**
** Run:
**
**   build/columnar-agg-bench ./columnar 10000000
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

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

static double run_query(sqlite3 *db, const char *zSql, double *pVal){
  sqlite3_stmt *pStmt = 0;
  double t0;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ) fatal(db, "prepare query", rc);
  t0 = now_ms();
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    *pVal = sqlite3_column_double(pStmt, 0);
    rc = sqlite3_step(pStmt);
  }
  if( rc!=SQLITE_DONE ) fatal(db, "query", rc);
  sqlite3_finalize(pStmt);
  return now_ms() - t0;
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

static void open_db(const char *zPath, sqlite3 **ppDb){
  int rc = sqlite3_open(zPath, ppDb);
  if( rc!=SQLITE_OK ) fatal(*ppDb, "open", rc);
}

static void setup_pragmas(sqlite3 *db){
  execsql(db,
      "PRAGMA journal_mode=OFF;"
      "PRAGMA synchronous=OFF;"
      "PRAGMA temp_store=MEMORY;"
      "PRAGMA locking_mode=EXCLUSIVE;"
      "PRAGMA cache_size=-200000");
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

int main(int argc, char **argv){
  const char *zExt = "./columnar";
  int iArg = 1;
  sqlite3_int64 nRow;
  sqlite3 *dbRow = 0;
  sqlite3 *dbCol = 0;
  double vRow = 0.0, vCol = 0.0;
  double tRow, tCol;
  const char *zRowDb = "/tmp/sqlite-row-agg-10m.db";
  const char *zColDb = "/tmp/sqlite-columnar-agg-10m.db";

  if( argc>1 && !is_integer_arg(argv[1]) ){
    zExt = argv[1];
    iArg++;
  }
  nRow = argc>iArg ? atoll(argv[iArg]) : 10000000;

  unlink(zRowDb);
  unlink(zColDb);

  open_db(zRowDb, &dbRow);
  open_db(zColDb, &dbCol);
  load_columnar(dbCol, zExt);
  setup_pragmas(dbRow);
  setup_pragmas(dbCol);

  execsql(dbRow, "CREATE TABLE r(v INTEGER)");
  execf(dbRow,
      "WITH RECURSIVE s(i) AS ("
      "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%lld"
      ") INSERT INTO r(rowid,v) SELECT i, i%%1000 FROM s",
      nRow);

  execsql(dbCol, "CREATE VIRTUAL TABLE c USING columnar(v INTEGER)");
  execf(dbCol,
      "WITH RECURSIVE s(i) AS ("
      "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%lld"
      ") INSERT INTO c__columnar_rowid(rid) SELECT i FROM s",
      nRow);
  execf(dbCol,
      "WITH RECURSIVE s(i) AS ("
      "  SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%lld"
      ") INSERT INTO c__columnar_c0(rid,v) SELECT i, i%%1000 FROM s",
      nRow);
  execsql(dbCol, "SELECT columnar_analyze('c')");

  sqlite3_close(dbRow);
  sqlite3_close(dbCol);
  open_db(zRowDb, &dbRow);
  open_db(zColDb, &dbCol);
  load_columnar(dbCol, zExt);
  setup_pragmas(dbRow);
  setup_pragmas(dbCol);

  printf("rows=%lld\n", (long long)nRow);
  printf("db_bytes row=%lld columnar=%lld\n",
      (long long)db_bytes(dbRow), (long long)db_bytes(dbCol));

  tRow = run_query(dbRow, "SELECT sum(v) FROM r", &vRow);
  tCol = run_query(dbCol, "SELECT columnar_sum('c','v')", &vCol);
  printf("sum row_ms=%.3f columnar_ms=%.3f speedup=%.2fx values=%.0f/%.0f\n",
      tRow, tCol, tCol>0.0 ? tRow/tCol : 0.0, vRow, vCol);

  tRow = run_query(dbRow, "SELECT avg(v) FROM r", &vRow);
  tCol = run_query(dbCol, "SELECT columnar_avg('c','v')", &vCol);
  printf("avg row_ms=%.3f columnar_ms=%.3f speedup=%.2fx values=%.6f/%.6f\n",
      tRow, tCol, tCol>0.0 ? tRow/tCol : 0.0, vRow, vCol);

  tRow = run_query(dbRow, "SELECT count(v) FROM r", &vRow);
  tCol = run_query(dbCol, "SELECT columnar_count('c','v')", &vCol);
  printf("count_value row_ms=%.3f columnar_ms=%.3f speedup=%.2fx values=%.0f/%.0f\n",
      tRow, tCol, tCol>0.0 ? tRow/tCol : 0.0, vRow, vCol);

  tRow = run_query(dbRow, "SELECT count(*) FROM r", &vRow);
  tCol = run_query(dbCol, "SELECT columnar_count('c')", &vCol);
  printf("count_rows row_ms=%.3f columnar_ms=%.3f speedup=%.2fx values=%.0f/%.0f\n",
      tRow, tCol, tCol>0.0 ? tRow/tCol : 0.0, vRow, vCol);

  sqlite3_close(dbRow);
  sqlite3_close(dbCol);
  return 0;
}
