/*
** Robustness tests for sqlite-columnar.
**
** This suite checks:
**   - specialized API equivalence against ordinary SQLite row-store queries
**   - rollback and savepoint behavior
**   - process-death recovery with an uncommitted transaction
**   - table/column names and mixed SQLite storage classes used as inputs
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

static void fatal(sqlite3 *db, const char *zMsg, int rc){
  fprintf(stderr, "%s: %s (%d)\n", zMsg, db ? sqlite3_errmsg(db) : "", rc);
  exit(1);
}

static void fail_compare(const char *zName, const char *zLeft, const char *zRight, const char *zLeftSql, const char *zRightSql){
  fprintf(stderr, "comparison failed: %s\nleft:  %s\nright: %s\nleft sql:  %s\nright sql: %s\n", zName, zLeft, zRight, zLeftSql, zRightSql);
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

static void execf(sqlite3 *db, const char *zFmt, ...){
  va_list ap;
  char *zSql;
  va_start(ap, zFmt);
  zSql = sqlite3_vmprintf(zFmt, ap);
  va_end(ap);
  if( zSql==0 ) fatal(db, "sqlite3_vmprintf", SQLITE_NOMEM);
  execsql(db, zSql);
  sqlite3_free(zSql);
}

static void open_db(const char *zPath, sqlite3 **ppDb){
  int rc = sqlite3_open(zPath, ppDb);
  if( rc!=SQLITE_OK ) fatal(*ppDb, "open", rc);
}

static void close_db(sqlite3 *db){
  int rc = sqlite3_close(db);
  if( rc!=SQLITE_OK ) fatal(db, "close", rc);
}

static void load_columnar(sqlite3 *db, const char *zExt){
  char *zErr = 0;
  int rc;
  sqlite3_enable_load_extension(db, 1);
  rc = sqlite3_load_extension(db, zExt, 0, &zErr);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "load extension: %s\n", zErr ? zErr : sqlite3_errmsg(db));
    sqlite3_free(zErr);
    exit(1);
  }
}

static void append_hex(sqlite3_str *pStr, const unsigned char *a, int n){
  int i;
  static const char zHex[] = "0123456789ABCDEF";
  for(i=0; i<n; i++){
    sqlite3_str_appendchar(pStr, 1, zHex[(a[i] >> 4) & 0x0f]);
    sqlite3_str_appendchar(pStr, 1, zHex[a[i] & 0x0f]);
  }
}

static char *query_result(sqlite3 *db, const char *zSql){
  sqlite3_stmt *pStmt = 0;
  sqlite3_str *pOut = sqlite3_str_new(db);
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  int nRow = 0;
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "prepare query_result failed\nSQL: %s\n", zSql);
    fatal(db, "prepare query_result", rc);
  }
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    int i;
    if( nRow++ ) sqlite3_str_appendchar(pOut, 1, '\n');
    for(i=0; i<sqlite3_column_count(pStmt); i++){
      if( i ) sqlite3_str_appendchar(pOut, 1, '|');
      switch( sqlite3_column_type(pStmt, i) ){
        case SQLITE_NULL:
          sqlite3_str_appendall(pOut, "NULL");
          break;
        case SQLITE_INTEGER:
          sqlite3_str_appendf(pOut, "I:%lld", sqlite3_column_int64(pStmt, i));
          break;
        case SQLITE_FLOAT:
          sqlite3_str_appendf(pOut, "F:%.17g", sqlite3_column_double(pStmt, i));
          break;
        case SQLITE_TEXT:
          sqlite3_str_appendf(pOut, "T:%s", sqlite3_column_text(pStmt, i));
          break;
        default:
          sqlite3_str_appendall(pOut, "B:");
          append_hex(pOut, sqlite3_column_blob(pStmt, i), sqlite3_column_bytes(pStmt, i));
          break;
      }
    }
  }
  if( rc!=SQLITE_DONE ) fatal(db, "step query_result", rc);
  sqlite3_finalize(pStmt);
  return sqlite3_str_finish(pOut);
}

static void compare_query(sqlite3 *db, const char *zName, const char *zRowSql, const char *zColSql){
  char *zRow = query_result(db, zRowSql);
  char *zCol = query_result(db, zColSql);
  if( zRow==0 || zCol==0 ) fatal(db, "query_result oom", SQLITE_NOMEM);
  if( strcmp(zRow, zCol)!=0 ) fail_compare(zName, zRow, zCol, zRowSql, zColSql);
  sqlite3_free(zRow);
  sqlite3_free(zCol);
}

static void compare_formatted(sqlite3 *db, const char *zName, char *zRowSql, char *zColSql){
  if( zRowSql==0 || zColSql==0 ) fatal(db, "compare_queryf oom", SQLITE_NOMEM);
  compare_query(db, zName, zRowSql, zColSql);
  sqlite3_free(zRowSql);
  sqlite3_free(zColSql);
}

static void expect_error(sqlite3 *db, const char *zName, const char *zSql){
  sqlite3_stmt *pStmt = 0;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc==SQLITE_OK ){
    int rcFinal;
    while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){}
    rcFinal = sqlite3_finalize(pStmt);
    if( rc==SQLITE_DONE && rcFinal==SQLITE_OK ){
      fprintf(stderr, "expected SQL error: %s\nSQL: %s\n", zName, zSql);
      exit(1);
    }
  }else{
    sqlite3_finalize(pStmt);
  }
}

static void check_integrity(sqlite3 *db){
  char *z = query_result(db, "PRAGMA integrity_check");
  if( z==0 ) fatal(db, "integrity_check oom", SQLITE_NOMEM);
  if( strcmp(z, "T:ok")!=0 ){
    fprintf(stderr, "integrity_check failed: %s\n", z);
    exit(1);
  }
  sqlite3_free(z);
}

static char *qid(const char *zName){
  return sqlite3_mprintf("\"%w\"", zName);
}

static void create_standard_pair(sqlite3 *db, const char *zRow, const char *zCol){
  char *zRowId = qid(zRow);
  char *zColId = qid(zCol);
  execf(db, "CREATE TABLE %s(id INTEGER, grp TEXT, k, v REAL, w REAL, flt INTEGER, txt TEXT, blobv BLOB)", zRowId);
  execf(db, "CREATE VIRTUAL TABLE %s USING columnar(id INTEGER, grp TEXT, k, v REAL, w REAL, flt INTEGER, txt TEXT, blobv BLOB)", zColId);
  sqlite3_free(zRowId);
  sqlite3_free(zColId);
}

static void bind_generated_row(sqlite3_stmt *pStmt, int i){
  unsigned char aBlob[3];
  sqlite3_bind_int(pStmt, 1, i);
  sqlite3_bind_int(pStmt, 2, i);
  if( i%5==0 ) sqlite3_bind_null(pStmt, 3);
  else if( i%5==1 ) sqlite3_bind_text(pStmt, 3, "alpha", -1, SQLITE_STATIC);
  else if( i%5==2 ) sqlite3_bind_text(pStmt, 3, "beta", -1, SQLITE_STATIC);
  else if( i%5==3 ) sqlite3_bind_text(pStmt, 3, "space key", -1, SQLITE_STATIC);
  else sqlite3_bind_text(pStmt, 3, "unicode-ish", -1, SQLITE_STATIC);
  if( i%4==0 ) sqlite3_bind_int64(pStmt, 4, (sqlite3_int64)(i%17));
  else if( i%4==1 ) sqlite3_bind_double(pStmt, 4, (double)(i%17));
  else if( i%4==2 ) sqlite3_bind_text(pStmt, 4, "text-key", -1, SQLITE_STATIC);
  else sqlite3_bind_null(pStmt, 4);
  if( i%7==0 ) sqlite3_bind_null(pStmt, 5);
  else sqlite3_bind_double(pStmt, 5, (double)((i%23)-11) * 1.25);
  if( i%11==0 ) sqlite3_bind_null(pStmt, 6);
  else sqlite3_bind_double(pStmt, 6, (double)(i%19) - 4.0);
  sqlite3_bind_int(pStmt, 7, i%50);
  if( i%6==0 ) sqlite3_bind_null(pStmt, 8);
  else sqlite3_bind_text(pStmt, 8, (i%2)==0 ? "even text" : "odd text", -1, SQLITE_STATIC);
  aBlob[0] = (unsigned char)i;
  aBlob[1] = (unsigned char)(i*3);
  aBlob[2] = (unsigned char)(255-i);
  if( i%9==0 ) sqlite3_bind_null(pStmt, 9);
  else sqlite3_bind_blob(pStmt, 9, aBlob, 3, SQLITE_TRANSIENT);
}

static void populate_standard_pair(sqlite3 *db, const char *zRow, const char *zCol, int nRow){
  sqlite3_stmt *pRow = 0;
  sqlite3_stmt *pCol = 0;
  char *zRowId = qid(zRow);
  char *zColId = qid(zCol);
  char *zSql = sqlite3_mprintf("INSERT INTO %s(rowid,id,grp,k,v,w,flt,txt,blobv) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)", zRowId);
  int i;
  if( zSql==0 ) fatal(db, "oom row insert sql", SQLITE_NOMEM);
  if( sqlite3_prepare_v2(db, zSql, -1, &pRow, 0)!=SQLITE_OK ) fatal(db, "prepare row insert", sqlite3_errcode(db));
  sqlite3_free(zSql);
  zSql = sqlite3_mprintf("INSERT INTO %s(rowid,id,grp,k,v,w,flt,txt,blobv) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)", zColId);
  if( zSql==0 ) fatal(db, "oom col insert sql", SQLITE_NOMEM);
  if( sqlite3_prepare_v2(db, zSql, -1, &pCol, 0)!=SQLITE_OK ) fatal(db, "prepare col insert", sqlite3_errcode(db));
  sqlite3_free(zSql);
  sqlite3_free(zRowId);
  sqlite3_free(zColId);
  for(i=1; i<=nRow; i++){
    bind_generated_row(pRow, i);
    if( sqlite3_step(pRow)!=SQLITE_DONE ) fatal(db, "insert row", sqlite3_errcode(db));
    sqlite3_reset(pRow);
    sqlite3_clear_bindings(pRow);
    bind_generated_row(pCol, i);
    if( sqlite3_step(pCol)!=SQLITE_DONE ) fatal(db, "insert col", sqlite3_errcode(db));
    sqlite3_reset(pCol);
    sqlite3_clear_bindings(pCol);
  }
  sqlite3_finalize(pRow);
  sqlite3_finalize(pCol);
}

static void analyze_columnar(sqlite3 *db, const char *zCol){
  execf(db, "SELECT columnar_analyze(%Q)", zCol);
}

static void compare_standard_pair(sqlite3 *db, const char *zRow, const char *zCol){
  char *zRowId = qid(zRow);
  compare_formatted(db, "sum(v)", sqlite3_mprintf("SELECT CASE WHEN sum(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',sum(v)) END FROM %s", zRowId), sqlite3_mprintf("SELECT CASE WHEN columnar_sum(%Q,'v') IS NULL THEN 'NULL' ELSE printf('%%.12g',columnar_sum(%Q,'v')) END", zCol, zCol));
  compare_formatted(db, "avg(v)", sqlite3_mprintf("SELECT CASE WHEN avg(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',avg(v)) END FROM %s", zRowId), sqlite3_mprintf("SELECT CASE WHEN columnar_avg(%Q,'v') IS NULL THEN 'NULL' ELSE printf('%%.12g',columnar_avg(%Q,'v')) END", zCol, zCol));
  compare_formatted(db, "count(v)", sqlite3_mprintf("SELECT count(v) FROM %s", zRowId), sqlite3_mprintf("SELECT columnar_count(%Q,'v')", zCol));
  compare_formatted(db, "count(*)", sqlite3_mprintf("SELECT count(*) FROM %s", zRowId), sqlite3_mprintf("SELECT columnar_count(%Q)", zCol));
  compare_formatted(db, "range sum", sqlite3_mprintf("SELECT CASE WHEN sum(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',sum(v)) END FROM %s WHERE flt BETWEEN 10 AND 40", zRowId), sqlite3_mprintf("SELECT CASE WHEN columnar_sum_where_range(%Q,'v','flt',10,40) IS NULL THEN 'NULL' ELSE printf('%%.12g',columnar_sum_where_range(%Q,'v','flt',10,40)) END", zCol, zCol));
  compare_formatted(db, "range avg", sqlite3_mprintf("SELECT CASE WHEN avg(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',avg(v)) END FROM %s WHERE flt BETWEEN 10 AND 40", zRowId), sqlite3_mprintf("SELECT CASE WHEN columnar_avg_where_range(%Q,'v','flt',10,40) IS NULL THEN 'NULL' ELSE printf('%%.12g',columnar_avg_where_range(%Q,'v','flt',10,40)) END", zCol, zCol));
  compare_formatted(db, "range count", sqlite3_mprintf("SELECT count(v) FROM %s WHERE flt BETWEEN 10 AND 40", zRowId), sqlite3_mprintf("SELECT columnar_count_where_range(%Q,'v','flt',10,40)", zCol));
  compare_formatted(db, "group sum", sqlite3_mprintf("SELECT quote(grp), CASE WHEN sum(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',sum(v)) END FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"sum\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"sum\") END FROM columnar_group_sum(%Q,'grp','v') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group avg", sqlite3_mprintf("SELECT quote(grp), CASE WHEN avg(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',avg(v)) END FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"avg\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"avg\") END FROM columnar_group_avg(%Q,'grp','v') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group count star", sqlite3_mprintf("SELECT quote(grp), count(*) FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), \"count\" FROM columnar_group_count(%Q,'grp') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group count value", sqlite3_mprintf("SELECT quote(grp), count(v) FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), \"count\" FROM columnar_group_count(%Q,'grp','v') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group min", sqlite3_mprintf("SELECT quote(grp), CASE WHEN min(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',min(v)) END FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"min\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"min\") END FROM columnar_group_min(%Q,'grp','v') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group max", sqlite3_mprintf("SELECT quote(grp), CASE WHEN max(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',max(v)) END FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"max\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"max\") END FROM columnar_group_max(%Q,'grp','v') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group sum avg count", sqlite3_mprintf("SELECT quote(grp), CASE WHEN sum(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',sum(v)) END, CASE WHEN avg(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',avg(v)) END, count(v) FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"sum\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"sum\") END, CASE WHEN \"avg\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"avg\") END, \"count\" FROM columnar_group_sum_avg_count(%Q,'grp','v') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group min max count", sqlite3_mprintf("SELECT quote(grp), CASE WHEN min(w) IS NULL THEN 'NULL' ELSE printf('%%.12g',min(w)) END, CASE WHEN max(w) IS NULL THEN 'NULL' ELSE printf('%%.12g',max(w)) END, count(w) FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"min\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"min\") END, CASE WHEN \"max\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"max\") END, \"count\" FROM columnar_group_min_max_count(%Q,'grp','w') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group range", sqlite3_mprintf("SELECT quote(grp), CASE WHEN max(v)-min(w) IS NULL THEN 'NULL' ELSE printf('%%.12g',max(v)-min(w)) END, CASE WHEN max(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',max(v)) END, CASE WHEN min(w) IS NULL THEN 'NULL' ELSE printf('%%.12g',min(w)) END, sum((v IS NOT NULL) OR (w IS NOT NULL)) FROM %s GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"range\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"range\") END, CASE WHEN \"max\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"max\") END, CASE WHEN \"min\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"min\") END, \"count\" FROM columnar_group_range(%Q,'grp','v','w') ORDER BY quote(k)", zCol));
  compare_formatted(db, "group where sum", sqlite3_mprintf("SELECT quote(grp), CASE WHEN sum(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',sum(v)) END FROM %s WHERE flt BETWEEN 10 AND 40 GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"sum\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"sum\") END FROM columnar_group_sum_where_range(%Q,'grp','v','flt',10,40) ORDER BY quote(k)", zCol));
  compare_formatted(db, "group where sum avg count", sqlite3_mprintf("SELECT quote(grp), CASE WHEN sum(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',sum(v)) END, CASE WHEN avg(v) IS NULL THEN 'NULL' ELSE printf('%%.12g',avg(v)) END, count(v) FROM %s WHERE flt BETWEEN 10 AND 40 GROUP BY grp ORDER BY quote(grp)", zRowId), sqlite3_mprintf("SELECT quote(k), CASE WHEN \"sum\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"sum\") END, CASE WHEN \"avg\" IS NULL THEN 'NULL' ELSE printf('%%.12g',\"avg\") END, \"count\" FROM columnar_group_sum_avg_count_where_range(%Q,'grp','v','flt',10,40) ORDER BY quote(k)", zCol));
  sqlite3_free(zRowId);
}

static void test_equivalence(sqlite3 *db){
  create_standard_pair(db, "r", "c");
  populate_standard_pair(db, "r", "c", 250);
  compare_standard_pair(db, "r", "c");
  analyze_columnar(db, "c");
  compare_standard_pair(db, "r", "c");
}

static void test_rollback(sqlite3 *db){
  create_standard_pair(db, "rr", "cc");
  populate_standard_pair(db, "rr", "cc", 80);
  analyze_columnar(db, "cc");
  compare_standard_pair(db, "rr", "cc");
  execsql(db, "BEGIN");
  execsql(db, "INSERT INTO cc(rowid,id,grp,k,v,w,flt,txt,blobv) VALUES(1001,1001,'mutated','m',999.0,10.0,20,'x',x'0102')");
  execsql(db, "UPDATE cc SET v=777.0, grp='mutated' WHERE rowid=5");
  execsql(db, "DELETE FROM cc WHERE rowid IN (6,7,8)");
  analyze_columnar(db, "cc");
  execsql(db, "ROLLBACK");
  compare_standard_pair(db, "rr", "cc");
  execsql(db, "SAVEPOINT s1");
  execsql(db, "INSERT INTO cc(rowid,id,grp,k,v,w,flt,txt,blobv) VALUES(1002,1002,'sp','m',123.0,4.0,30,'x',x'0102')");
  execsql(db, "ROLLBACK TO s1");
  execsql(db, "RELEASE s1");
  compare_standard_pair(db, "rr", "cc");
}

static void test_fuzz_names(sqlite3 *db){
  const char *zRow = "row table";
  const char *zCol = "col table__columnar_base";
  char *zRowId = qid(zRow);
  char *zColId = qid(zCol);
  int i;
  execf(db, "CREATE TABLE %s(\"group key\", \"value column\" REAL, \"filter column\" INTEGER, \"payload blob\" BLOB)", zRowId);
  execf(db, "CREATE VIRTUAL TABLE %s USING columnar(\"group key\", \"value column\" REAL, \"filter column\" INTEGER, \"payload blob\" BLOB)", zColId);
  sqlite3_free(zRowId);
  sqlite3_free(zColId);
  for(i=0; i<90; i++){
    const char *zKey = (i%3)==0 ? "k one" : ((i%3)==1 ? "select" : "with space");
    if( i%10==0 ){
      execf(db, "INSERT INTO \"row table\" VALUES(NULL,NULL,%d,x'%02x%02x')", i%31, i&255, (i*7)&255);
      execf(db, "INSERT INTO \"col table__columnar_base\" VALUES(NULL,NULL,%d,x'%02x%02x')", i%31, i&255, (i*7)&255);
    }else{
      execf(db, "INSERT INTO \"row table\" VALUES(%Q,%.17g,%d,x'%02x%02x')", zKey, (double)(i%17)-3.5, i%31, i&255, (i*7)&255);
      execf(db, "INSERT INTO \"col table__columnar_base\" VALUES(%Q,%.17g,%d,x'%02x%02x')", zKey, (double)(i%17)-3.5, i%31, i&255, (i*7)&255);
    }
  }
  compare_query(db, "fuzz sum before analyze", "SELECT CASE WHEN sum(\"value column\") IS NULL THEN 'NULL' ELSE printf('%.12g',sum(\"value column\")) END FROM \"row table\"", "SELECT CASE WHEN columnar_sum('col table__columnar_base','value column') IS NULL THEN 'NULL' ELSE printf('%.12g',columnar_sum('col table__columnar_base','value column')) END");
  analyze_columnar(db, "col table__columnar_base");
  compare_query(db, "fuzz sum after analyze", "SELECT CASE WHEN sum(\"value column\") IS NULL THEN 'NULL' ELSE printf('%.12g',sum(\"value column\")) END FROM \"row table\"", "SELECT CASE WHEN columnar_sum('col table__columnar_base','value column') IS NULL THEN 'NULL' ELSE printf('%.12g',columnar_sum('col table__columnar_base','value column')) END");
  compare_query(db, "fuzz range", "SELECT count(\"value column\") FROM \"row table\" WHERE \"filter column\" BETWEEN 7 AND 19", "SELECT columnar_count_where_range('col table__columnar_base','value column','filter column',7,19)");
  compare_query(db, "fuzz group", "SELECT quote(\"group key\"), CASE WHEN sum(\"value column\") IS NULL THEN 'NULL' ELSE printf('%.12g',sum(\"value column\")) END FROM \"row table\" GROUP BY \"group key\" ORDER BY quote(\"group key\")", "SELECT quote(k), CASE WHEN \"sum\" IS NULL THEN 'NULL' ELSE printf('%.12g',\"sum\") END FROM columnar_group_sum('col table__columnar_base','group key','value column') ORDER BY quote(k)");
}

static void test_api_fuzz(sqlite3 *db){
  expect_error(db, "missing table scalar", "SELECT columnar_sum('missing_table','v')");
  expect_error(db, "missing column scalar", "SELECT columnar_sum('c','missing_column')");
  expect_error(db, "null table scalar", "SELECT columnar_sum(NULL,'v')");
  expect_error(db, "missing table analyze", "SELECT columnar_analyze('missing_table')");
  expect_error(db, "missing key group", "SELECT * FROM columnar_group_sum('c','missing_column','v')");
  expect_error(db, "missing value group", "SELECT * FROM columnar_group_sum('c','grp','missing_column')");
  expect_error(db, "missing filter range", "SELECT columnar_sum_where_range('c','v','missing_column',1,2)");
  expect_error(db, "wrong table type", "SELECT columnar_count(123,'v')");
}

#ifndef _WIN32
static void create_crash_db(const char *zDb, const char *zExt){
  sqlite3 *db = 0;
  unlink(zDb);
  open_db(zDb, &db);
  load_columnar(db, zExt);
  execsql(db, "PRAGMA journal_mode=DELETE");
  execsql(db, "PRAGMA synchronous=FULL");
  create_standard_pair(db, "rcrash", "ccrash");
  populate_standard_pair(db, "rcrash", "ccrash", 120);
  analyze_columnar(db, "ccrash");
  close_db(db);
}

static void crash_child(const char *zDb, const char *zExt){
  sqlite3 *db = 0;
  open_db(zDb, &db);
  load_columnar(db, zExt);
  execsql(db, "PRAGMA journal_mode=DELETE");
  execsql(db, "PRAGMA synchronous=FULL");
  execsql(db, "BEGIN IMMEDIATE");
  execsql(db, "INSERT INTO ccrash(rowid,id,grp,k,v,w,flt,txt,blobv) VALUES(999,999,'crash','k',999.0,9.0,10,'boom',x'aa')");
  execsql(db, "UPDATE ccrash SET v=-999.0 WHERE rowid=4");
  execsql(db, "DELETE FROM ccrash WHERE rowid=5");
  analyze_columnar(db, "ccrash");
  _exit(0);
}

static void test_crash_recovery(const char *zExt){
  char zDb[256];
  sqlite3 *db = 0;
  pid_t pid;
  int status = 0;
  snprintf(zDb, sizeof(zDb), "/tmp/sqlite-columnar-robustness-%ld.db", (long)getpid());
  create_crash_db(zDb, zExt);
  pid = fork();
  if( pid<0 ){
    perror("fork");
    exit(1);
  }
  if( pid==0 ) crash_child(zDb, zExt);
  if( waitpid(pid, &status, 0)<0 ){
    perror("waitpid");
    exit(1);
  }
  if( !WIFEXITED(status) ){
    fprintf(stderr, "crash child did not exit normally\n");
    exit(1);
  }
  open_db(zDb, &db);
  load_columnar(db, zExt);
  check_integrity(db);
  compare_standard_pair(db, "rcrash", "ccrash");
  close_db(db);
  unlink(zDb);
}
#endif

int main(int argc, char **argv){
  const char *zExt = argc>1 ? argv[1] : "./columnar";
  sqlite3 *db = 0;
  open_db(":memory:", &db);
  load_columnar(db, zExt);
  execsql(db, "PRAGMA trusted_schema=OFF");
  test_equivalence(db);
  test_api_fuzz(db);
  test_rollback(db);
  test_fuzz_names(db);
  close_db(db);
#ifndef _WIN32
  test_crash_recovery(zExt);
#endif
  printf("columnar robustness tests passed\n");
  return 0;
}
