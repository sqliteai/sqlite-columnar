/*
** Repeatable performance-variance benchmark for sqlite-columnar.
**
** This benchmark builds multiple deterministic analytics datasets, runs each
** query several times, verifies row-store/columnar checksums, and reports
** median and p95 timings.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct Dataset Dataset;
typedef struct QueryDef QueryDef;
typedef struct SampleStats SampleStats;
typedef struct QueryResult QueryResult;

struct Dataset {
  const char *zName;
  sqlite3_int64 nRow;
  int nPayload;
};

struct QueryDef {
  const char *zName;
  const char *zRowSql;
  const char *zColSql;
};

struct SampleStats {
  double rMedian;
  double rP95;
};

struct QueryResult {
  double rMs;
  unsigned long long uHash;
  int nRow;
};

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

static QueryDef aStaticQuery[] = {
  {
    "full_sum_v1",
    "SELECT sum(v1) FROM r",
    "SELECT columnar_sum('c','v1')"
  },
  {
    "full_avg_v3",
    "SELECT avg(v3) FROM r",
    "SELECT columnar_avg('c','v3')"
  },
  {
    "full_count_v1",
    "SELECT count(v1) FROM r",
    "SELECT columnar_count('c','v1')"
  },
  {
    "generic_group_id1_sum_v1",
    "SELECT id1, sum(v1) FROM r GROUP BY id1 ORDER BY id1",
    "SELECT id1, sum(v1) FROM c GROUP BY id1 ORDER BY id1"
  },
  {
    "specialized_group_sum_id1_v1",
    "SELECT id1, sum(v1) FROM r GROUP BY id1 ORDER BY id1",
    "SELECT k, \"sum\" FROM columnar_group_sum('c','id1','v1') ORDER BY k"
  },
  {
    "specialized_group_count_id1",
    "SELECT id1, count(*) FROM r GROUP BY id1 ORDER BY id1",
    "SELECT k, \"count\" FROM columnar_group_count('c','id1') ORDER BY k"
  },
  {
    "specialized_group_sum_avg_count_id1_v1",
    "SELECT id1, sum(v1), avg(v1), count(v1) FROM r GROUP BY id1 ORDER BY id1",
    "SELECT k, \"sum\", \"avg\", \"count\" FROM columnar_group_sum_avg_count('c','id1','v1') ORDER BY k"
  },
  {
    "specialized_group_min_max_count_id3_v1",
    "SELECT id3, min(v1), max(v1), count(v1) FROM r GROUP BY id3 ORDER BY id3",
    "SELECT k, \"min\", \"max\", \"count\" FROM columnar_group_min_max_count('c','id3','v1') ORDER BY k"
  }
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

static void open_db(const char *zPath, sqlite3 **ppDb){
  int rc = sqlite3_open(zPath, ppDb);
  if( rc!=SQLITE_OK ) fatal(*ppDb, "open", rc);
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

static void setup_pragmas(sqlite3 *db){
  execsql(db,
      "PRAGMA journal_mode=OFF;"
      "PRAGMA synchronous=OFF;"
      "PRAGMA temp_store=MEMORY;"
      "PRAGMA locking_mode=EXCLUSIVE;"
      "PRAGMA cache_size=-300000");
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

static sqlite3_int64 db_bytes(sqlite3 *db){
  sqlite3_int64 nPage = int64_query(db, "PRAGMA page_count");
  sqlite3_int64 nPageSize = int64_query(db, "PRAGMA page_size");
  return nPage * nPageSize;
}

static int is_integer_arg(const char *z){
  if( z==0 || z[0]==0 ) return 0;
  while( z[0]>='0' && z[0]<='9' ) z++;
  return z[0]==0;
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
      (long long)nRow, zExpr);
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
        (long long)nRow, i, zExpr);
    sqlite3_free(zExpr);
    if( zSql==0 ) fatal(db, "oom", SQLITE_NOMEM);
    execsql(db, zSql);
    sqlite3_free(zSql);
  }
  execsql(db, "COMMIT");
}

static void hash_bytes(unsigned long long *pHash, const void *pData, int nData){
  const unsigned char *a = (const unsigned char*)pData;
  int i;
  for(i=0; i<nData; i++){
    *pHash ^= (unsigned long long)a[i];
    *pHash *= 1099511628211ULL;
  }
}

static void hash_int(unsigned long long *pHash, sqlite3_int64 v){
  char z[32];
  sqlite3_snprintf(sizeof(z), z, "%lld", (long long)v);
  hash_bytes(pHash, z, (int)strlen(z));
}

static void hash_value(unsigned long long *pHash, sqlite3_stmt *pStmt, int iCol){
  int eType = sqlite3_column_type(pStmt, iCol);
  switch( eType ){
    case SQLITE_NULL:
      hash_bytes(pHash, "N", 1);
      break;
    case SQLITE_INTEGER:
    case SQLITE_FLOAT: {
      char z[64];
      sqlite3_snprintf(sizeof(z), z, "%.17g", sqlite3_column_double(pStmt, iCol));
      hash_bytes(pHash, "D", 1);
      hash_bytes(pHash, z, (int)strlen(z));
      break;
    }
    case SQLITE_TEXT: {
      int n = sqlite3_column_bytes(pStmt, iCol);
      const unsigned char *z = sqlite3_column_text(pStmt, iCol);
      hash_bytes(pHash, "T", 1);
      hash_int(pHash, n);
      hash_bytes(pHash, ":", 1);
      hash_bytes(pHash, z, n);
      break;
    }
    default: {
      int n = sqlite3_column_bytes(pStmt, iCol);
      const void *p = sqlite3_column_blob(pStmt, iCol);
      hash_bytes(pHash, "B", 1);
      hash_int(pHash, n);
      hash_bytes(pHash, ":", 1);
      hash_bytes(pHash, p, n);
      break;
    }
  }
}

static QueryResult run_query(sqlite3 *db, const char *zSql){
  sqlite3_stmt *pStmt = 0;
  QueryResult s = {0.0, 1469598103934665603ULL, 0};
  double t0;
  int rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  if( rc!=SQLITE_OK ){
    fprintf(stderr, "prepare failed\nSQL: %s\n", zSql);
    fatal(db, "prepare query", rc);
  }
  t0 = now_ms();
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    int i;
    int nCol = sqlite3_column_count(pStmt);
    s.nRow++;
    hash_bytes(&s.uHash, "R", 1);
    hash_int(&s.uHash, s.nRow);
    hash_bytes(&s.uHash, "C", 1);
    hash_int(&s.uHash, nCol);
    for(i=0; i<nCol; i++){
      hash_bytes(&s.uHash, "|", 1);
      hash_int(&s.uHash, i);
      hash_bytes(&s.uHash, "=", 1);
      hash_value(&s.uHash, pStmt, i);
    }
  }
  if( rc!=SQLITE_DONE ) fatal(db, "query", rc);
  sqlite3_finalize(pStmt);
  s.rMs = now_ms() - t0;
  return s;
}

static int double_cmp(const void *a, const void *b){
  double x = *(const double*)a;
  double y = *(const double*)b;
  if( x<y ) return -1;
  if( x>y ) return 1;
  return 0;
}

static SampleStats sample_stats(double *a, int n){
  SampleStats s = {0.0, 0.0};
  int iP95;
  qsort(a, (size_t)n, sizeof(double), double_cmp);
  if( n%2 ){
    s.rMedian = a[n/2];
  }else{
    s.rMedian = (a[n/2-1] + a[n/2]) / 2.0;
  }
  iP95 = (95*n + 99) / 100 - 1;
  if( iP95<0 ) iP95 = 0;
  if( iP95>=n ) iP95 = n-1;
  s.rP95 = a[iP95];
  return s;
}

static void run_one_benchmark(sqlite3 *dbRow, sqlite3 *dbCol, const Dataset *pDataset, const char *zName, const char *zRowSql, const char *zColSql, int nRepeat){
  double *aRow = sqlite3_malloc64(sizeof(double) * (sqlite3_uint64)nRepeat);
  double *aCol = sqlite3_malloc64(sizeof(double) * (sqlite3_uint64)nRepeat);
  QueryResult sWarmRow;
  QueryResult sWarmCol;
  QueryResult sRow;
  QueryResult sCol;
  SampleStats sRowStats;
  SampleStats sColStats;
  int i;
  if( aRow==0 || aCol==0 ) fatal(dbCol, "oom samples", SQLITE_NOMEM);

  sWarmRow = run_query(dbRow, zRowSql);
  sWarmCol = run_query(dbCol, zColSql);
  if( sWarmRow.nRow!=sWarmCol.nRow || sWarmRow.uHash!=sWarmCol.uHash ){
    fprintf(stderr, "result mismatch during warmup for %s: rows=%d/%d hash=%016llx/%016llx\n", zName, sWarmRow.nRow, sWarmCol.nRow, sWarmRow.uHash, sWarmCol.uHash);
    exit(1);
  }

  for(i=0; i<nRepeat; i++){
    sRow = run_query(dbRow, zRowSql);
    sCol = run_query(dbCol, zColSql);
    if( sRow.nRow!=sCol.nRow || sRow.uHash!=sCol.uHash ){
      fprintf(stderr, "result mismatch for %s repeat=%d: rows=%d/%d hash=%016llx/%016llx\n", zName, i, sRow.nRow, sCol.nRow, sRow.uHash, sCol.uHash);
      exit(1);
    }
    aRow[i] = sRow.rMs;
    aCol[i] = sCol.rMs;
  }

  sRowStats = sample_stats(aRow, nRepeat);
  sColStats = sample_stats(aCol, nRepeat);
  printf("query,%s,%lld,%d,%s,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%d,%016llx\n",
      pDataset->zName,
      (long long)pDataset->nRow,
      pDataset->nPayload,
      zName,
      sRowStats.rMedian,
      sRowStats.rP95,
      sColStats.rMedian,
      sColStats.rP95,
      sColStats.rMedian>0.0 ? sRowStats.rMedian/sColStats.rMedian : 0.0,
      sColStats.rP95>0.0 ? sRowStats.rP95/sColStats.rP95 : 0.0,
      sWarmRow.nRow,
      sWarmRow.uHash);
  fflush(stdout);
  sqlite3_free(aRow);
  sqlite3_free(aCol);
}

static void run_dynamic_benchmark(sqlite3 *dbRow, sqlite3 *dbCol, const Dataset *pDataset, const char *zName, char *zRowSql, char *zColSql, int nRepeat){
  if( zRowSql==0 || zColSql==0 ) fatal(dbCol, "sqlite3_mprintf", SQLITE_NOMEM);
  run_one_benchmark(dbRow, dbCol, pDataset, zName, zRowSql, zColSql, nRepeat);
  sqlite3_free(zRowSql);
  sqlite3_free(zColSql);
}

static void run_dataset(const char *zExt, const Dataset *pDataset, int iDataset, int nRepeat){
  sqlite3 *dbRow = 0;
  sqlite3 *dbCol = 0;
  char zRowDb[256];
  char zColDb[256];
  double t0;
  double rPopulateRow;
  double rPopulateCol;
  double rAnalyze;
  sqlite3_int64 iRangeLo = pDataset->nRow / 100;
  sqlite3_int64 iRangeHi = iRangeLo + pDataset->nRow / 100;
  sqlite3_int64 nChunk;
  sqlite3_int64 nSelectedChunk;
  sqlite3_int64 nFullChunk;
  char *zSql;
  int i;

  if( iRangeLo<1 ) iRangeLo = 1;
  if( iRangeHi>pDataset->nRow ) iRangeHi = pDataset->nRow;

  snprintf(zRowDb, sizeof(zRowDb), "/tmp/sqlite-columnar-variance-%ld-%d-row.db", (long)getpid(), iDataset);
  snprintf(zColDb, sizeof(zColDb), "/tmp/sqlite-columnar-variance-%ld-%d-columnar.db", (long)getpid(), iDataset);
  unlink(zRowDb);
  unlink(zColDb);

  open_db(zRowDb, &dbRow);
  open_db(zColDb, &dbCol);
  load_columnar(dbCol, zExt);
  setup_pragmas(dbRow);
  setup_pragmas(dbCol);

  t0 = now_ms();
  populate_row(dbRow, pDataset->nRow, pDataset->nPayload);
  rPopulateRow = now_ms() - t0;

  t0 = now_ms();
  populate_columnar(dbCol, pDataset->nRow, pDataset->nPayload);
  rPopulateCol = now_ms() - t0;

  t0 = now_ms();
  execsql(dbCol, "SELECT columnar_analyze('c')");
  rAnalyze = now_ms() - t0;

  nChunk = int64_query(dbCol, "SELECT count(*) FROM c__columnar_chunk WHERE col=0");
  zSql = sqlite3_mprintf(
      "SELECT count(*) FROM c__columnar_chunk"
      " WHERE col=0 AND min IS NOT NULL AND max>=%lld AND min<=%lld",
      (long long)iRangeLo, (long long)iRangeHi);
  if( zSql==0 ) fatal(dbCol, "sqlite3_mprintf", SQLITE_NOMEM);
  nSelectedChunk = int64_query(dbCol, zSql);
  sqlite3_free(zSql);
  zSql = sqlite3_mprintf(
      "SELECT count(*) FROM c__columnar_chunk"
      " WHERE col=0 AND nonnull_count=row_count AND min>=%lld AND max<=%lld",
      (long long)iRangeLo, (long long)iRangeHi);
  if( zSql==0 ) fatal(dbCol, "sqlite3_mprintf", SQLITE_NOMEM);
  nFullChunk = int64_query(dbCol, zSql);
  sqlite3_free(zSql);

  printf("dataset,%s,%lld,%d,%.3f,%.3f,%.3f,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
      pDataset->zName,
      (long long)pDataset->nRow,
      pDataset->nPayload,
      rPopulateRow,
      rPopulateCol,
      rAnalyze,
      (long long)db_bytes(dbRow),
      (long long)db_bytes(dbCol),
      (long long)iRangeLo,
      (long long)iRangeHi,
      (long long)nSelectedChunk,
      (long long)nFullChunk,
      (long long)nChunk);
  fflush(stdout);

  for(i=0; i<(int)(sizeof(aStaticQuery)/sizeof(aStaticQuery[0])); i++){
    run_one_benchmark(dbRow, dbCol, pDataset, aStaticQuery[i].zName, aStaticQuery[i].zRowSql, aStaticQuery[i].zColSql, nRepeat);
  }

  run_dynamic_benchmark(dbRow, dbCol, pDataset, "clustered_ts_filter_specialized",
      sqlite3_mprintf(
        "SELECT sum(v1), avg(v3), count(v1) FROM r WHERE ts BETWEEN %lld AND %lld",
        (long long)iRangeLo, (long long)iRangeHi),
      sqlite3_mprintf(
        "SELECT columnar_sum_where_range('c','v1','ts',%lld,%lld),"
        " columnar_avg_where_range('c','v3','ts',%lld,%lld),"
        " columnar_count_where_range('c','v1','ts',%lld,%lld)",
        (long long)iRangeLo, (long long)iRangeHi,
        (long long)iRangeLo, (long long)iRangeHi,
        (long long)iRangeLo, (long long)iRangeHi),
      nRepeat);

  run_dynamic_benchmark(dbRow, dbCol, pDataset, "clustered_ts_group_sum_avg_count_specialized",
      sqlite3_mprintf(
        "SELECT id1, sum(v1), avg(v1), count(v1) FROM r WHERE ts BETWEEN %lld AND %lld GROUP BY id1 ORDER BY id1",
        (long long)iRangeLo, (long long)iRangeHi),
      sqlite3_mprintf(
        "SELECT k, \"sum\", \"avg\", \"count\" FROM columnar_group_sum_avg_count_where_range('c','id1','v1','ts',%lld,%lld) ORDER BY k",
        (long long)iRangeLo, (long long)iRangeHi),
      nRepeat);

  sqlite3_close(dbRow);
  sqlite3_close(dbCol);
  unlink(zRowDb);
  unlink(zColDb);
}

static Dataset parse_dataset_arg(const char *zArg, int iDataset){
  Dataset s;
  char *zCopy = sqlite3_mprintf("%s", zArg);
  char *azPart[3] = {0, 0, 0};
  char *zTok;
  int nPart = 0;
  if( zCopy==0 ) exit(1);
  zTok = strtok(zCopy, ":");
  while( zTok!=0 && nPart<3 ){
    azPart[nPart++] = zTok;
    zTok = strtok(0, ":");
  }
  if( nPart==3 ){
    s.zName = sqlite3_mprintf("%s", azPart[0]);
    s.nRow = atoll(azPart[1]);
    s.nPayload = atoi(azPart[2]);
  }else if( nPart==2 ){
    s.zName = sqlite3_mprintf("dataset%d", iDataset);
    s.nRow = atoll(azPart[0]);
    s.nPayload = atoi(azPart[1]);
  }else{
    fprintf(stderr, "invalid dataset spec: %s\nexpected name:rows:payload or rows:payload\n", zArg);
    exit(1);
  }
  sqlite3_free(zCopy);
  if( s.zName==0 || s.nRow<1 || s.nPayload<0 ){
    fprintf(stderr, "invalid dataset spec: %s\n", zArg);
    exit(1);
  }
  return s;
}

int main(int argc, char **argv){
  const char *zExt = "./columnar";
  int iArg = 1;
  int nRepeat;
  int i;
  Dataset aDefault[] = {
    {"small", 10000, 64},
    {"medium", 50000, 128},
    {"wide", 50000, 512}
  };

  if( argc>1 && !is_integer_arg(argv[1]) ){
    zExt = argv[1];
    iArg++;
  }
  nRepeat = argc>iArg ? atoi(argv[iArg]) : 9;
  if( nRepeat<1 ) nRepeat = 1;
  iArg++;

  printf("# sqlite-columnar performance variance benchmark\n");
  printf("# repeats=%d warmup=1\n", nRepeat);
  printf("dataset,dataset_name,rows,payload,populate_row_ms,populate_columnar_ms,columnar_analyze_ms,row_bytes,columnar_bytes,range_lo,range_hi,selected_chunks,full_cover_chunks,total_chunks\n");
  printf("query,dataset_name,rows,payload,metric,row_median_ms,row_p95_ms,columnar_median_ms,columnar_p95_ms,speedup_median,speedup_p95,result_rows,result_hash\n");
  fflush(stdout);

  if( iArg<argc ){
    int iDataset = 0;
    for(i=iArg; i<argc; i++){
      Dataset s = parse_dataset_arg(argv[i], iDataset);
      run_dataset(zExt, &s, iDataset, nRepeat);
      sqlite3_free((void*)s.zName);
      iDataset++;
    }
  }else{
    for(i=0; i<(int)(sizeof(aDefault)/sizeof(aDefault[0])); i++){
      run_dataset(zExt, &aDefault[i], i, nRepeat);
    }
  }
  return 0;
}
