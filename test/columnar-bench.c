/*
** Benchmark for columnar.c.
**
** Build from the repository root:
**
**   make build/columnar-bench
**
** Run:
**
**   build/columnar-bench ./columnar 100000 256
**
** Arguments are: extension path, row count, payload text bytes.
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

static void fill_payload(char *z, int n){
  int i;
  for(i=0; i<n; i++) z[i] = (char)('a' + (i%23));
  z[n] = 0;
}

static void populate(sqlite3 *db, const char *zTable, int nRow, int nPayload){
  sqlite3_stmt *pStmt = 0;
  char *zSql;
  char *zPayload = malloc((size_t)nPayload + 1);
  int rc;
  int i;
  if( zPayload==0 ) exit(1);
  fill_payload(zPayload, nPayload);
  zSql = sqlite3_mprintf(
      "INSERT INTO %s(k,v0,v1,p0,p1,p2,p3,p4,p5,p6) "
      "VALUES(?1,?2,?3,?4,?4,?4,?4,?4,?4,?4)",
      zTable);
  if( zSql==0 ) exit(1);
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ) fatal(db, "prepare insert", rc);
  execsql(db, "BEGIN");
  for(i=1; i<=nRow; i++){
    sqlite3_bind_int(pStmt, 1, i);
    sqlite3_bind_int(pStmt, 2, i%1000);
    sqlite3_bind_int(pStmt, 3, (i*7)%1000);
    sqlite3_bind_text(pStmt, 4, zPayload, nPayload, SQLITE_STATIC);
    rc = sqlite3_step(pStmt);
    if( rc!=SQLITE_DONE ) fatal(db, "insert", rc);
    sqlite3_reset(pStmt);
    sqlite3_clear_bindings(pStmt);
  }
  execsql(db, "COMMIT");
  sqlite3_finalize(pStmt);
  free(zPayload);
}

static double run_query(sqlite3 *db, const char *zSql, double *pValue){
  sqlite3_stmt *pStmt = 0;
  double t0;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ) fatal(db, "prepare query", rc);
  t0 = now_ms();
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    *pValue = sqlite3_column_double(pStmt, 0);
    rc = sqlite3_step(pStmt);
  }
  if( rc!=SQLITE_DONE ) fatal(db, "query", rc);
  sqlite3_finalize(pStmt);
  return now_ms() - t0;
}

static sqlite3_int64 file_size(sqlite3 *db){
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

int main(int argc, char **argv){
  const char *zExt = argc>1 ? argv[1] : "./columnar";
  int nRow = argc>2 ? atoi(argv[2]) : 100000;
  int nPayload = argc>3 ? atoi(argv[3]) : 256;
  sqlite3 *dbRow = 0;
  sqlite3 *dbCol = 0;
  char *zErr = 0;
  double vRow = 0.0, vCol = 0.0;
  double tRow, tCol;
  int rc;

  unlink("/tmp/sqlite-row-bench.db");
  unlink("/tmp/sqlite-columnar-bench.db");

  rc = sqlite3_open("/tmp/sqlite-row-bench.db", &dbRow);
  if( rc!=SQLITE_OK ) fatal(dbRow, "open row db", rc);
  rc = sqlite3_open("/tmp/sqlite-columnar-bench.db", &dbCol);
  if( rc!=SQLITE_OK ) fatal(dbCol, "open columnar db", rc);

  if( zExt[0]!=0 ){
    sqlite3_enable_load_extension(dbCol, 1);
    rc = sqlite3_load_extension(dbCol, zExt, 0, &zErr);
    if( rc!=SQLITE_OK ){
      fprintf(stderr, "load extension: %s\n", zErr ? zErr : sqlite3_errmsg(dbCol));
      sqlite3_free(zErr);
      return 1;
    }
  }

  execsql(dbRow, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=MEMORY");
  execsql(dbCol, "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=MEMORY");
  execsql(dbRow,
      "CREATE TABLE r(k INTEGER, v0 INTEGER, v1 INTEGER, "
      "p0 TEXT, p1 TEXT, p2 TEXT, p3 TEXT, p4 TEXT, p5 TEXT, p6 TEXT)");
  execsql(dbCol,
      "CREATE VIRTUAL TABLE c USING columnar(k INTEGER, v0 INTEGER, v1 INTEGER, "
      "p0 TEXT, p1 TEXT, p2 TEXT, p3 TEXT, p4 TEXT, p5 TEXT, p6 TEXT)");

  populate(dbRow, "r", nRow, nPayload);
  populate(dbCol, "c", nRow, nPayload);
  execsql(dbCol, "SELECT columnar_analyze('c')");

  tRow = run_query(dbRow, "SELECT sum(v0) FROM r", &vRow);
  tCol = run_query(dbCol, "SELECT sum(v0) FROM c", &vCol);
  printf("rows=%d payload_bytes=%d\n", nRow, nPayload);
  printf("query=sum(v0) row_ms=%.3f columnar_ms=%.3f speedup=%.2fx values=%.0f/%.0f\n",
      tRow, tCol, tCol>0.0 ? tRow/tCol : 0.0, vRow, vCol);

  tCol = run_query(dbCol, "SELECT columnar_sum('c','v0')", &vCol);
  printf("query=columnar_sum(v0) specialized_ms=%.3f vs_row_speedup=%.2fx value=%.0f\n",
      tCol, tCol>0.0 ? tRow/tCol : 0.0, vCol);

  tRow = run_query(dbRow, "SELECT sum(v0+v1) FROM r WHERE v1 BETWEEN 100 AND 900", &vRow);
  tCol = run_query(dbCol, "SELECT sum(v0+v1) FROM c WHERE v1 BETWEEN 100 AND 900", &vCol);
  printf("query=filter_sum row_ms=%.3f columnar_ms=%.3f speedup=%.2fx values=%.0f/%.0f\n",
      tRow, tCol, tCol>0.0 ? tRow/tCol : 0.0, vRow, vCol);

  printf("db_bytes row=%lld columnar=%lld\n",
      (long long)file_size(dbRow), (long long)file_size(dbCol));

  sqlite3_close(dbRow);
  sqlite3_close(dbCol);
  return 0;
}
