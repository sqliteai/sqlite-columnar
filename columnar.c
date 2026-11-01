/*
**
** Column-oriented storage for SQLite, implemented as a virtual table module.
** This is intentionally isolated from the core pager/btree code: each virtual
** table owns one shadow rowid table plus one shadow table per declared column.
**
** Usage:
**
**   .load ./columnar
**   CREATE VIRTUAL TABLE c USING columnar(a INTEGER, b TEXT, c REAL);
**   INSERT INTO c VALUES(1,'one',1.5);
**   SELECT sum(a) FROM c;
**
** The table is rowid-addressed.  Full scans stream only the columns marked
** used by sqlite3_index_info.colUsed, so analytical queries over a subset of
** columns avoid reading the other column btrees.
**
** Created by Marco Bambini on 08/05/26.
**
*/
#if defined(BUILD_sqlite) && !defined(SQLITE_CORE)
# define SQLITE_CORE 1
#endif
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SQLITE_PRIVATE
# define SQLITE_PRIVATE
#endif

#ifndef COLUMNAR_VERSION
# define COLUMNAR_VERSION "1.1.1"
#endif

#ifndef SQLITE_OMIT_VIRTUALTABLE

typedef struct ColumnarVtab ColumnarVtab;
typedef struct ColumnarCursor ColumnarCursor;
typedef struct ColumnarAnalytic ColumnarAnalytic;
typedef struct ColumnarRangeAnalytic ColumnarRangeAnalytic;
typedef struct ColumnarGroupAgg ColumnarGroupAgg;
typedef struct ColumnarGroupVtab ColumnarGroupVtab;
typedef struct ColumnarGroupCursor ColumnarGroupCursor;
typedef struct ColumnarGroupEntry ColumnarGroupEntry;
typedef struct ColumnarTableRef ColumnarTableRef;
typedef struct ColumnarValue ColumnarValue;
typedef struct ColumnarSum ColumnarSum;

static sqlite3_module columnarModule;
static sqlite3_module columnarGroupModule;
static sqlite3_module columnarGroupWhereModule;

// PRAGMA MARK: - Types -

struct ColumnarVtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *zDb;
  char *zName;
  char *zRowTab;
  char *zStatTab;
  char *zChunkTab;
  char *zDirtyTab;
  char *zMetaTab;
  char **azColTab;
  int nCol;
};

struct ColumnarCursor {
  sqlite3_vtab_cursor base;
  sqlite3_int64 iRowid;
  sqlite3_uint64 colUsed;
  int bEof;
  int bPoint;
  int iDriver;
  sqlite3_stmt *pRowStmt;
  sqlite3_stmt **apStmt;
};

struct ColumnarTableRef {
  char *zDb;
  char *zTab;
};

struct ColumnarAnalytic {
  int eOp;
};

struct ColumnarRangeAnalytic {
  int eOp;
};

struct ColumnarGroupAgg {
  int eOp;
  int nOut;
  int nValArg;
  int nValHidden;
  const char *zDecl;
  const char *zErrName;
};

struct ColumnarGroupVtab {
  sqlite3_vtab base;
  sqlite3 *db;
  const ColumnarGroupAgg *pAgg;
};

/* A copy of an sqlite3_value, owned by whatever structure embeds it.
**
** eType is one of the SQLITE_NULL/INTEGER/FLOAT/TEXT/BLOB storage classes, or
** zero when no value has been stored yet.  rNum carries the value as SQL
** arithmetic sees it, so that numeric text behaves the way it would in an
** ordinary expression.
*/
struct ColumnarValue {
  int eType;
  int nVal;                  /* Byte count, for TEXT and BLOB */
  double rNum;
  union {
    sqlite3_int64 i;
    double r;
    void *p;                 /* TEXT and BLOB payload, sqlite3_malloc64'd */
  } u;
};

/* A running sum that stays exact for as long as every value added is an
** integer, the way SQLite's own sum() does.  It drops to floating point when a
** non-integer value arrives or when the integer accumulator would overflow.
*/
struct ColumnarSum {
  sqlite3_int64 nVal;        /* Number of non-NULL values summed */
  sqlite3_int64 iSum;        /* Exact sum, valid while bInt */
  double rSum;               /* Compensated sum; add rErr for the result */
  double rErr;
  int bInt;
};

struct ColumnarGroupEntry {
  unsigned int uKeyHash;
  unsigned char bUsed;
  ColumnarValue sKey;
  ColumnarValue sMin;
  ColumnarValue sMax;
  ColumnarSum sSum;
  sqlite3_int64 nCount;
};

struct ColumnarGroupCursor {
  sqlite3_vtab_cursor base;
  ColumnarGroupEntry *aEntry;
  int nAlloc;
  int nEntry;
  int iEntry;
  sqlite3_int64 iRowid;
};

#define COLUMNAR_ANALYTIC_SUM    1
#define COLUMNAR_ANALYTIC_AVG    2
#define COLUMNAR_ANALYTIC_COUNT  3

#define COLUMNAR_CHUNK_SIZE 65536

#define COLUMNAR_GROUP_SUM              1
#define COLUMNAR_GROUP_AVG              2
#define COLUMNAR_GROUP_COUNT            3
#define COLUMNAR_GROUP_SUM_AVG_COUNT    4
#define COLUMNAR_GROUP_MIN              5
#define COLUMNAR_GROUP_MAX              6
#define COLUMNAR_GROUP_MIN_MAX_COUNT    7
#define COLUMNAR_GROUP_RANGE            8

#define COLUMNAR_GROUP_IDX_VAL1  0x01
#define COLUMNAR_GROUP_IDX_VAL2  0x02

// PRAGMA MARK: - Operation Descriptors -

static const ColumnarAnalytic columnarSumOp = { COLUMNAR_ANALYTIC_SUM };
static const ColumnarAnalytic columnarAvgOp = { COLUMNAR_ANALYTIC_AVG };
static const ColumnarAnalytic columnarCountOp = { COLUMNAR_ANALYTIC_COUNT };
static const ColumnarRangeAnalytic columnarRangeSumOp = { COLUMNAR_ANALYTIC_SUM };
static const ColumnarRangeAnalytic columnarRangeAvgOp = { COLUMNAR_ANALYTIC_AVG };
static const ColumnarRangeAnalytic columnarRangeCountOp = { COLUMNAR_ANALYTIC_COUNT };

static const ColumnarGroupAgg columnarGroupSumAgg = {
  COLUMNAR_GROUP_SUM, 2, 1, 1,
  "CREATE TABLE x(k,sum,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_sum"
};
static const ColumnarGroupAgg columnarGroupAvgAgg = {
  COLUMNAR_GROUP_AVG, 2, 1, 1,
  "CREATE TABLE x(k,avg,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_avg"
};
static const ColumnarGroupAgg columnarGroupCountAgg = {
  COLUMNAR_GROUP_COUNT, 2, 0, 1,
  "CREATE TABLE x(k,count,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_count"
};
static const ColumnarGroupAgg columnarGroupSumAvgCountAgg = {
  COLUMNAR_GROUP_SUM_AVG_COUNT, 4, 1, 1,
  "CREATE TABLE x(k,sum,avg,count,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_sum_avg_count"
};
static const ColumnarGroupAgg columnarGroupMinAgg = {
  COLUMNAR_GROUP_MIN, 2, 1, 1,
  "CREATE TABLE x(k,min,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_min"
};
static const ColumnarGroupAgg columnarGroupMaxAgg = {
  COLUMNAR_GROUP_MAX, 2, 1, 1,
  "CREATE TABLE x(k,max,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_max"
};
static const ColumnarGroupAgg columnarGroupMinMaxCountAgg = {
  COLUMNAR_GROUP_MIN_MAX_COUNT, 4, 1, 1,
  "CREATE TABLE x(k,min,max,count,tab HIDDEN,keycol HIDDEN,valcol HIDDEN)",
  "columnar_group_min_max_count"
};
static const ColumnarGroupAgg columnarGroupRangeAgg = {
  COLUMNAR_GROUP_RANGE, 5, 2, 2,
  "CREATE TABLE x(k,range,max,min,count,tab HIDDEN,keycol HIDDEN,maxcol HIDDEN,mincol HIDDEN)",
  "columnar_group_range"
};
static const ColumnarGroupAgg columnarGroupSumWhereAgg = {
  COLUMNAR_GROUP_SUM, 2, 1, 1,
  "CREATE TABLE x(k,sum,tab HIDDEN,keycol HIDDEN,valcol HIDDEN,filtercol HIDDEN,lo HIDDEN,hi HIDDEN)",
  "columnar_group_sum_where_range"
};
static const ColumnarGroupAgg columnarGroupSumAvgCountWhereAgg = {
  COLUMNAR_GROUP_SUM_AVG_COUNT, 4, 1, 1,
  "CREATE TABLE x(k,sum,avg,count,tab HIDDEN,keycol HIDDEN,valcol HIDDEN,filtercol HIDDEN,lo HIDDEN,hi HIDDEN)",
  "columnar_group_sum_avg_count_where_range"
};

// PRAGMA MARK: - Virtual Table Setup -

static void columnarFreeVtab(ColumnarVtab *p){
  int i;
  if( p==0 ) return;
  sqlite3_free(p->zDb);
  sqlite3_free(p->zName);
  sqlite3_free(p->zRowTab);
  sqlite3_free(p->zStatTab);
  sqlite3_free(p->zChunkTab);
  sqlite3_free(p->zDirtyTab);
  sqlite3_free(p->zMetaTab);
  if( p->azColTab ){
    for(i=0; i<p->nCol; i++) sqlite3_free(p->azColTab[i]);
    sqlite3_free(p->azColTab);
  }
  sqlite3_free(p);
}

static int columnarIsIdentChar(char c){
  return ((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_');
}

static int columnarIsSpace(char c){
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f' || c=='\v';
}

/* Step past the column name at the start of a column definition. */
static const char *columnarSkipColumnName(const char *z){
  while( columnarIsSpace(z[0]) ) z++;
  if( z[0]=='"' || z[0]=='`' || z[0]=='[' ){
    char q = z[0]=='[' ? ']' : z[0];
    z++;
    while( z[0] ){
      if( z[0]==q ){
        /* Inside "..." and `...` a doubled quote is an escaped quote. */
        if( q!=']' && z[1]==q ){
          z += 2;
          continue;
        }
        z++;
        break;
      }
      z++;
    }
  }else{
    while( columnarIsIdentChar(z[0]) ) z++;
  }
  return z;
}

/* True if the n-byte token at z starts a column constraint rather than
** continuing the declared type. */
static int columnarIsConstraintWord(const char *z, int n){
  static const char *const azStop[] = {
    "CONSTRAINT", "PRIMARY", "NOT", "NULL", "UNIQUE", "CHECK",
    "DEFAULT", "COLLATE", "REFERENCES", "GENERATED", "AS"
  };
  int i;
  for(i=0; i<(int)(sizeof(azStop)/sizeof(azStop[0])); i++){
    if( (int)strlen(azStop[i])==n && sqlite3_strnicmp(z, azStop[i], n)==0 ){
      return 1;
    }
  }
  return 0;
}

static int columnarTypeContains(const char *z, int n, const char *zWord){
  int nWord = (int)strlen(zWord);
  int i;
  for(i=0; i+nWord<=n; i++){
    if( sqlite3_strnicmp(&z[i], zWord, nWord)==0 ) return 1;
  }
  return 0;
}

/* Isolate the declared type of a column definition: the token sequence between
** the column name and the first column constraint.  Length is returned in
** *pnType, which is zero when the column has no type at all. */
static const char *columnarDeclaredType(const char *zDef, int *pnType){
  const char *z = columnarSkipColumnName(zDef);
  const char *zStart;
  const char *zEnd;
  int nDepth = 0;
  while( columnarIsSpace(z[0]) ) z++;
  zStart = z;
  zEnd = z;
  while( z[0] ){
    if( z[0]=='(' ){
      nDepth++;
      z++;
    }else if( z[0]==')' ){
      if( nDepth>0 ) nDepth--;
      z++;
      zEnd = z;
    }else if( nDepth>0 ){
      z++;
    }else if( columnarIsSpace(z[0]) ){
      z++;
    }else if( z[0]=='"' || z[0]=='`' || z[0]=='[' || z[0]=='\'' ){
      /* A type name may be quoted, as in: c "DOUBLE PRECISION".  The quotes
      ** are kept in the span, which is harmless for the keyword search. */
      char q = z[0]=='[' ? ']' : z[0];
      z++;
      while( z[0] ){
        if( z[0]==q ){
          if( q!=']' && z[1]==q ){
            z += 2;
            continue;
          }
          z++;
          break;
        }
        z++;
      }
      zEnd = z;
    }else if( columnarIsIdentChar(z[0]) ){
      const char *zTok = z;
      while( columnarIsIdentChar(z[0]) ) z++;
      if( columnarIsConstraintWord(zTok, (int)(z-zTok)) ) break;
      zEnd = z;
    }else{
      break;
    }
  }
  *pnType = (int)(zEnd - zStart);
  return zStart;
}

/* The type keyword to declare a shadow column with, so that the shadow column
** ends up with the same affinity SQLite would give the user's column.  Giving
** the shadow column its real affinity is what makes stored values -- and the
** comparisons the analytic helpers run directly against them -- behave exactly
** as they would in an ordinary table.  Returns "" for a column with no type,
** which leaves the shadow column with BLOB affinity, i.e. no conversion.
**
** The rules below are SQLite's own, applied in the same order.
*/
static const char *columnarAffinityType(const char *zDef){
  const char *zType;
  int nType = 0;
  if( zDef==0 ) return "";
  zType = columnarDeclaredType(zDef, &nType);
  if( nType<=0 ) return "";
  if( columnarTypeContains(zType, nType, "INT") ) return "INTEGER";
  if( columnarTypeContains(zType, nType, "CHAR")
   || columnarTypeContains(zType, nType, "CLOB")
   || columnarTypeContains(zType, nType, "TEXT")
  ){
    return "TEXT";
  }
  if( columnarTypeContains(zType, nType, "BLOB") ) return "BLOB";
  if( columnarTypeContains(zType, nType, "REAL")
   || columnarTypeContains(zType, nType, "FLOA")
   || columnarTypeContains(zType, nType, "DOUB")
  ){
    return "REAL";
  }
  return "NUMERIC";
}

/* True if a module argument is a table constraint rather than a column
** definition.  SQLite's grammar makes this unambiguous: a table constraint
** always opens with one of these keywords, and none of them is accepted as a
** bare column name, so a quoted first token is always a column.
**
** The distinction matters because SQLite sizes xUpdate's argument vector from
** the number of columns it declared.  Counting a constraint as a column would
** make the write path read one element past the end of that vector.
*/
static int columnarIsTableConstraint(const char *zDef){
  static const char *const azKw[] = {
    "CONSTRAINT", "PRIMARY", "UNIQUE", "CHECK", "FOREIGN"
  };
  const char *z = zDef;
  const char *zTok;
  int n;
  int i;

  if( z==0 ) return 0;
  while( columnarIsSpace(z[0]) ) z++;
  if( z[0]=='"' || z[0]=='`' || z[0]=='[' || z[0]=='\'' ) return 0;
  zTok = z;
  while( columnarIsIdentChar(z[0]) ) z++;
  n = (int)(z - zTok);
  if( n==0 ) return 0;
  for(i=0; i<(int)(sizeof(azKw)/sizeof(azKw[0])); i++){
    if( (int)strlen(azKw[i])==n && sqlite3_strnicmp(zTok, azKw[i], n)==0 ){
      return 1;
    }
  }
  return 0;
}

/* Number of module arguments that actually declare a column. */
static int columnarCountColumns(int argc, const char *const *argv){
  int n = 0;
  int i;
  for(i=3; i<argc; i++){
    if( !columnarIsTableConstraint(argv[i]) ) n++;
  }
  return n;
}

/* The module argument declaring column iCol, or 0 if there is none. */
static const char *columnarColumnArg(int argc, const char *const *argv, int iCol){
  int n = 0;
  int i;
  for(i=3; i<argc; i++){
    if( columnarIsTableConstraint(argv[i]) ) continue;
    if( n==iCol ) return argv[i];
    n++;
  }
  return 0;
}

static char *columnarDeclaration(sqlite3 *db, int argc, const char *const *argv){
  sqlite3_str *pStr = sqlite3_str_new(db);
  int i;
  sqlite3_str_appendall(pStr, "CREATE TABLE x(");
  for(i=3; i<argc; i++){
    if( i>3 ) sqlite3_str_appendall(pStr, ",");
    sqlite3_str_appendall(pStr, argv[i]);
  }
  sqlite3_str_appendall(pStr, ")");
  return sqlite3_str_finish(pStr);
}

static int columnarExec(sqlite3 *db, char **pzErr, const char *zSql){
  char *zErr = 0;
  int rc = sqlite3_exec(db, zSql, 0, 0, &zErr);
  if( rc!=SQLITE_OK ){
    if( pzErr ){
      *pzErr = sqlite3_mprintf("%s", zErr ? zErr : sqlite3_errmsg(db));
    }
    sqlite3_free(zErr);
  }
  return rc;
}

static int columnarPrepare(sqlite3 *db, sqlite3_stmt **ppStmt, char **pzErr, const char *zFmt, ...){
  va_list ap;
  char *zSql;
  int rc;
  va_start(ap, zFmt);
  zSql = sqlite3_vmprintf(zFmt, ap);
  va_end(ap);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = sqlite3_prepare_v2(db, zSql, -1, ppStmt, 0);
  if( rc!=SQLITE_OK && pzErr ){
    *pzErr = sqlite3_mprintf("%s: %s", sqlite3_errmsg(db), zSql);
  }
  sqlite3_free(zSql);
  return rc;
}

static int columnarInit(sqlite3 *db, int bCreate, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr){
  ColumnarVtab *p = 0;
  char *zDecl = 0;
  int rc;
  int i;

  if( argc<4 || columnarCountColumns(argc, argv)<1 ){
    *pzErr = sqlite3_mprintf("columnar requires at least one column");
    return SQLITE_ERROR;
  }

  zDecl = columnarDeclaration(db, argc, argv);
  if( zDecl==0 ) return SQLITE_NOMEM;
  rc = sqlite3_declare_vtab(db, zDecl);
  sqlite3_free(zDecl);
  if( rc!=SQLITE_OK ) return rc;

  p = sqlite3_malloc64(sizeof(*p));
  if( p==0 ) return SQLITE_NOMEM;
  memset(p, 0, sizeof(*p));
  p->db = db;
  /* Must match what sqlite3_declare_vtab() just declared: a table constraint
  ** among the module arguments declares no column. */
  p->nCol = columnarCountColumns(argc, argv);
  p->zDb = sqlite3_mprintf("%s", argv[1]);
  p->zName = sqlite3_mprintf("%s", argv[2]);
  p->zRowTab = sqlite3_mprintf("%s__columnar_rowid", argv[2]);
  p->zStatTab = sqlite3_mprintf("%s__columnar_stat", argv[2]);
  p->zChunkTab = sqlite3_mprintf("%s__columnar_chunk", argv[2]);
  p->zDirtyTab = sqlite3_mprintf("%s__columnar_dirty", argv[2]);
  p->zMetaTab = sqlite3_mprintf("%s__columnar_meta", argv[2]);
  p->azColTab = sqlite3_malloc64(sizeof(char*) * p->nCol);
  /* Zero it before anything can fail: the cleanup below frees every element,
  ** so an uninitialized array would have it free arbitrary values. */
  if( p->azColTab ) memset(p->azColTab, 0, sizeof(char*) * p->nCol);
  if( p->zDb==0 || p->zName==0 || p->zRowTab==0
   || p->zStatTab==0 || p->zChunkTab==0 || p->zDirtyTab==0
   || p->zMetaTab==0
   || p->azColTab==0
  ){
    columnarFreeVtab(p);
    return SQLITE_NOMEM;
  }
  for(i=0; i<p->nCol; i++){
    p->azColTab[i] = sqlite3_mprintf("%s__columnar_c%d", argv[2], i);
    if( p->azColTab[i]==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
  }

  if( bCreate ){
    char *zSql;
    zSql = sqlite3_mprintf("CREATE TABLE %Q.%Q(rid INTEGER PRIMARY KEY)", p->zDb, p->zRowTab);
    if( zSql==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ){
      columnarFreeVtab(p);
      return rc;
    }
    zSql = sqlite3_mprintf("CREATE TABLE %Q.%Q(" "col INTEGER PRIMARY KEY," "row_count INTEGER NOT NULL," "nonnull_count INTEGER NOT NULL," "sum," "min," "max" ")", p->zDb, p->zStatTab);
    if( zSql==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ){
      columnarFreeVtab(p);
      return rc;
    }
    zSql = sqlite3_mprintf("CREATE TABLE %Q.%Q(" "col INTEGER NOT NULL," "chunk INTEGER NOT NULL," "rid_min INTEGER NOT NULL," "rid_max INTEGER NOT NULL," "row_count INTEGER NOT NULL," "nonnull_count INTEGER NOT NULL," "sum," "min," "max," "PRIMARY KEY(col,chunk)" ")", p->zDb, p->zChunkTab);
    if( zSql==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ){
      columnarFreeVtab(p);
      return rc;
    }
    zSql = sqlite3_mprintf("CREATE TABLE %Q.%Q(" "col INTEGER NOT NULL," "chunk INTEGER NOT NULL," "PRIMARY KEY(col,chunk)" ")", p->zDb, p->zDirtyTab);
    if( zSql==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ){
      columnarFreeVtab(p);
      return rc;
    }
    zSql = sqlite3_mprintf("CREATE TABLE %Q.%Q(" "k TEXT PRIMARY KEY," "v INTEGER NOT NULL" ")", p->zDb, p->zMetaTab);
    if( zSql==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ){
      columnarFreeVtab(p);
      return rc;
    }
    zSql = sqlite3_mprintf("INSERT INTO %Q.%Q(k,v) VALUES" "('format_version',1)," "('chunk_size',%d)," "('ncol',%d)," "('row_count',0)," "('chunk_count',0)," "('dirty_count',0)," "('stats_valid',0)", p->zDb, p->zMetaTab, COLUMNAR_CHUNK_SIZE, p->nCol);
    if( zSql==0 ){
      columnarFreeVtab(p);
      return SQLITE_NOMEM;
    }
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
    if( rc!=SQLITE_OK ){
      columnarFreeVtab(p);
      return rc;
    }
    for(i=0; i<p->nCol; i++){
      zSql = sqlite3_mprintf("CREATE TABLE %Q.%Q(rid INTEGER PRIMARY KEY, v %s)", p->zDb, p->azColTab[i], columnarAffinityType(columnarColumnArg(argc, argv, i)));
      if( zSql==0 ){
        columnarFreeVtab(p);
        return SQLITE_NOMEM;
      }
      rc = columnarExec(db, pzErr, zSql);
      sqlite3_free(zSql);
      if( rc!=SQLITE_OK ){
        columnarFreeVtab(p);
        return rc;
      }
    }
  }

  *ppVtab = (sqlite3_vtab*)p;
  return SQLITE_OK;
}

static int columnarCreate(sqlite3 *db, void *pAux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr){
  (void)pAux;
  return columnarInit(db, 1, argc, argv, ppVtab, pzErr);
}

static int columnarConnect(sqlite3 *db, void *pAux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr){
  (void)pAux;
  return columnarInit(db, 0, argc, argv, ppVtab, pzErr);
}

static int columnarDisconnect(sqlite3_vtab *pVtab){
  columnarFreeVtab((ColumnarVtab*)pVtab);
  return SQLITE_OK;
}

static int columnarTableExists(sqlite3 *db, const char *zDb, const char *zTab, int *pbExists);

static int columnarDestroy(sqlite3_vtab *pVtab){
  ColumnarVtab *p = (ColumnarVtab*)pVtab;
  sqlite3 *db = p->db;
  char *zErr = 0;
  int rc = SQLITE_OK;
  int i;
  for(i=0; rc==SQLITE_OK && i<p->nCol; i++){
    char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, p->azColTab[i]);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
    }
  }
  /* Releases up to 1.0.0 counted table constraints as columns and so created
  ** one shadow column table too many.  Drop any that are still there, or they
  ** would be left behind and block recreating a table of this name. */
  for(i=p->nCol; rc==SQLITE_OK; i++){
    char *zTab = sqlite3_mprintf("%s__columnar_c%d", p->zName, i);
    int bExists = 0;
    if( zTab==0 ){
      rc = SQLITE_NOMEM;
      break;
    }
    rc = columnarTableExists(db, p->zDb, zTab, &bExists);
    if( rc==SQLITE_OK && bExists ){
      char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, zTab);
      if( zSql==0 ){
        rc = SQLITE_NOMEM;
      }else{
        rc = columnarExec(db, &zErr, zSql);
        sqlite3_free(zSql);
      }
    }
    sqlite3_free(zTab);
    if( !bExists ) break;
  }
  if( rc==SQLITE_OK ){
    char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, p->zStatTab);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
    }
  }
  if( rc==SQLITE_OK ){
    char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, p->zChunkTab);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
    }
  }
  if( rc==SQLITE_OK ){
    char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, p->zDirtyTab);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
    }
  }
  if( rc==SQLITE_OK ){
    char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, p->zMetaTab);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
    }
  }
  if( rc==SQLITE_OK ){
    char *zSql = sqlite3_mprintf("DROP TABLE IF EXISTS %Q.%Q", p->zDb, p->zRowTab);
    if( zSql==0 ){
      rc = SQLITE_NOMEM;
    }else{
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
    }
  }
  if( rc!=SQLITE_OK ){
    /* SQLite only clears its pointer to this vtab when xDestroy succeeds, and
    ** disconnects it later itself.  Freeing it here would leave that pointer
    ** dangling, to be used and freed a second time during the rollback. */
    sqlite3_free(p->base.zErrMsg);
    p->base.zErrMsg = zErr;
    return rc;
  }
  sqlite3_free(zErr);
  columnarDisconnect(pVtab);
  return SQLITE_OK;
}

static int columnarOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor){
  ColumnarVtab *p = (ColumnarVtab*)pVtab;
  ColumnarCursor *pCur;
  pCur = sqlite3_malloc64(sizeof(*pCur));
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->iDriver = -1;
  pCur->apStmt = sqlite3_malloc64(sizeof(sqlite3_stmt*) * p->nCol);
  if( pCur->apStmt==0 ){
    sqlite3_free(pCur);
    return SQLITE_NOMEM;
  }
  memset(pCur->apStmt, 0, sizeof(sqlite3_stmt*) * p->nCol);
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

static void columnarCursorClear(ColumnarCursor *pCur, int nCol){
  int i;
  sqlite3_finalize(pCur->pRowStmt);
  pCur->pRowStmt = 0;
  if( pCur->apStmt ){
    for(i=0; i<nCol; i++){
      sqlite3_finalize(pCur->apStmt[i]);
      pCur->apStmt[i] = 0;
    }
  }
  pCur->bEof = 1;
  pCur->bPoint = 0;
  pCur->iDriver = -1;
}

static int columnarClose(sqlite3_vtab_cursor *cur){
  ColumnarCursor *pCur = (ColumnarCursor*)cur;
  ColumnarVtab *p = (ColumnarVtab*)cur->pVtab;
  columnarCursorClear(pCur, p->nCol);
  sqlite3_free(pCur->apStmt);
  sqlite3_free(pCur);
  return SQLITE_OK;
}

static int columnarUsed(sqlite3_uint64 m, int iCol){
  if( iCol>=63 ) return (m & (((sqlite3_uint64)1)<<63))!=0;
  return (m & (((sqlite3_uint64)1)<<iCol))!=0;
}

static int columnarParseU64(const char *z, sqlite3_uint64 *pVal){
  sqlite3_uint64 v = 0;
  if( z==0 || z[0]==0 ){
    *pVal = 0;
    return SQLITE_OK;
  }
  while( z[0]>='0' && z[0]<='9' ){
    v = v*10 + (sqlite3_uint64)(z[0]-'0');
    z++;
  }
  *pVal = v;
  return SQLITE_OK;
}

static int columnarAdvance(ColumnarCursor *pCur){
  ColumnarVtab *p = (ColumnarVtab*)pCur->base.pVtab;
  int rc;
  int i;

  if( pCur->bPoint ){
    pCur->bEof = 1;
    return SQLITE_OK;
  }

  if( pCur->iDriver<0 ){
    rc = sqlite3_step(pCur->pRowStmt);
    if( rc==SQLITE_ROW ){
      pCur->iRowid = sqlite3_column_int64(pCur->pRowStmt, 0);
      pCur->bEof = 0;
      return SQLITE_OK;
    }
    pCur->bEof = 1;
    return rc==SQLITE_DONE ? SQLITE_OK : rc;
  }

  rc = sqlite3_step(pCur->apStmt[pCur->iDriver]);
  if( rc==SQLITE_DONE ){
    pCur->bEof = 1;
    return SQLITE_OK;
  }
  if( rc!=SQLITE_ROW ) return rc;
  pCur->iRowid = sqlite3_column_int64(pCur->apStmt[pCur->iDriver], 0);
  for(i=0; i<p->nCol; i++){
    if( i==pCur->iDriver || pCur->apStmt[i]==0 ) continue;
    rc = sqlite3_step(pCur->apStmt[i]);
    if( rc!=SQLITE_ROW ) return rc==SQLITE_DONE ? SQLITE_CORRUPT : rc;
    if( sqlite3_column_int64(pCur->apStmt[i], 0)!=pCur->iRowid ){
      return SQLITE_CORRUPT;
    }
  }
  pCur->bEof = 0;
  return SQLITE_OK;
}

static int columnarFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv){
  ColumnarCursor *pCur = (ColumnarCursor*)cur;
  ColumnarVtab *p = (ColumnarVtab*)cur->pVtab;
  char *zErr = 0;
  int rc;
  int i;
  sqlite3_int64 iRowid = 0;

  columnarCursorClear(pCur, p->nCol);
  columnarParseU64(idxStr, &pCur->colUsed);
  pCur->bPoint = (idxNum & 1)!=0;

  if( pCur->bPoint ){
    assert( argc==1 );
    iRowid = sqlite3_value_int64(argv[0]);
    rc = columnarPrepare(p->db, &pCur->pRowStmt, &zErr, "SELECT rid FROM %Q.%Q WHERE rid=?1", p->zDb, p->zRowTab);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pCur->pRowStmt, 1, iRowid);
      rc = sqlite3_step(pCur->pRowStmt);
      if( rc==SQLITE_ROW ){
        pCur->iRowid = iRowid;
        pCur->bEof = 0;
        rc = SQLITE_OK;
      }else{
        pCur->bEof = 1;
        rc = rc==SQLITE_DONE ? SQLITE_OK : rc;
      }
    }
    sqlite3_free(zErr);
    return rc;
  }

  for(i=0; i<p->nCol; i++){
    if( columnarUsed(pCur->colUsed, i) ){
      pCur->iDriver = i;
      break;
    }
  }
  if( pCur->iDriver<0 ){
    rc = columnarPrepare(p->db, &pCur->pRowStmt, &zErr, "SELECT rid FROM %Q.%Q ORDER BY rid", p->zDb, p->zRowTab);
  }else{
    rc = SQLITE_OK;
    for(i=0; rc==SQLITE_OK && i<p->nCol; i++){
      if( !columnarUsed(pCur->colUsed, i) ) continue;
      rc = columnarPrepare(p->db, &pCur->apStmt[i], &zErr, "SELECT rid, v FROM %Q.%Q ORDER BY rid", p->zDb, p->azColTab[i]);
    }
  }
  if( rc!=SQLITE_OK ){
    sqlite3_free(cur->pVtab->zErrMsg);
    cur->pVtab->zErrMsg = zErr;
    return rc;
  }
  sqlite3_free(zErr);
  return columnarAdvance(pCur);
}

// PRAGMA MARK: - Cursor Output -

static int columnarNext(sqlite3_vtab_cursor *cur){
  return columnarAdvance((ColumnarCursor*)cur);
}

static int columnarEof(sqlite3_vtab_cursor *cur){
  return ((ColumnarCursor*)cur)->bEof;
}

static int columnarLookupValue(ColumnarVtab *p, sqlite3_int64 iRowid, int iCol, sqlite3_context *ctx){
  sqlite3_stmt *pStmt = 0;
  char *zErr = 0;
  int rc = columnarPrepare(p->db, &pStmt, &zErr, "SELECT v FROM %Q.%Q WHERE rid=?1", p->zDb, p->azColTab[iCol]);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iRowid);
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      sqlite3_result_value(ctx, sqlite3_column_value(pStmt, 0));
      rc = SQLITE_OK;
    }else if( rc==SQLITE_DONE ){
      sqlite3_result_null(ctx);
      rc = SQLITE_OK;
    }
  }
  sqlite3_finalize(pStmt);
  sqlite3_free(zErr);
  return rc;
}

static int columnarColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i){
  ColumnarCursor *pCur = (ColumnarCursor*)cur;
  ColumnarVtab *p = (ColumnarVtab*)cur->pVtab;
  if( i<0 || i>=p->nCol ){
    sqlite3_result_null(ctx);
    return SQLITE_OK;
  }
  if( !pCur->bPoint && pCur->apStmt[i] ){
    sqlite3_result_value(ctx, sqlite3_column_value(pCur->apStmt[i], 1));
    return SQLITE_OK;
  }
  return columnarLookupValue(p, pCur->iRowid, i, ctx);
}

static int columnarRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  *pRowid = ((ColumnarCursor*)cur)->iRowid;
  return SQLITE_OK;
}

static int columnarBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  int i;
  int iRowidCons = -1;
  (void)tab;
  for(i=0; i<pIdxInfo->nConstraint; i++){
    const struct sqlite3_index_constraint *pCons = &pIdxInfo->aConstraint[i];
    if( pCons->usable && pCons->iColumn<0
     && pCons->op==SQLITE_INDEX_CONSTRAINT_EQ
    ){
      iRowidCons = i;
      break;
    }
  }
  if( iRowidCons>=0 ){
    pIdxInfo->aConstraintUsage[iRowidCons].argvIndex = 1;
    pIdxInfo->aConstraintUsage[iRowidCons].omit = 1;
    pIdxInfo->idxNum = 1;
    pIdxInfo->estimatedRows = 1;
    pIdxInfo->estimatedCost = 5.0;
  }else{
    pIdxInfo->idxNum = 0;
    pIdxInfo->estimatedRows = 1000000;
    pIdxInfo->estimatedCost = 1000000.0;
  }
  pIdxInfo->idxStr = sqlite3_mprintf("%llu", (unsigned long long)pIdxInfo->colUsed);
  if( pIdxInfo->idxStr==0 ) return SQLITE_NOMEM;
  pIdxInfo->needToFreeIdxStr = 1;
  return SQLITE_OK;
}

static int columnarStepDone(sqlite3_stmt *pStmt){
  int rc = sqlite3_step(pStmt);
  return rc==SQLITE_DONE ? SQLITE_OK : rc;
}

// PRAGMA MARK: - Stored Values -

#define COLUMNAR_MAX_I64  ((sqlite3_int64)0x7fffffffffffffffLL)
#define COLUMNAR_MIN_I64  (-COLUMNAR_MAX_I64 - 1)

static int columnarDoubleToInt64(double r, sqlite3_int64 *pi);

/* Take a private copy of pVal.  Either succeeds or leaves *p untouched. */
static int columnarValueSave(ColumnarValue *p, sqlite3_value *pVal){
  int eType = sqlite3_value_type(pVal);
  const void *aData = 0;
  int nData = 0;

  if( eType==SQLITE_TEXT ){
    aData = (const void*)sqlite3_value_text(pVal);
    nData = sqlite3_value_bytes(pVal);
  }else if( eType!=SQLITE_NULL && eType!=SQLITE_INTEGER && eType!=SQLITE_FLOAT ){
    eType = SQLITE_BLOB;
    aData = sqlite3_value_blob(pVal);
    nData = sqlite3_value_bytes(pVal);
  }
  if( eType==SQLITE_TEXT || eType==SQLITE_BLOB ){
    void *pNew = 0;
    if( nData>0 ){
      if( aData==0 ) return SQLITE_NOMEM;
      pNew = sqlite3_malloc64((sqlite3_uint64)nData);
      if( pNew==0 ) return SQLITE_NOMEM;
      memcpy(pNew, aData, (size_t)nData);
    }
    p->u.p = pNew;
    p->nVal = nData;
  }else if( eType==SQLITE_INTEGER ){
    p->u.i = sqlite3_value_int64(pVal);
    p->nVal = 0;
  }else{
    p->u.r = sqlite3_value_double(pVal);
    p->nVal = 0;
  }
  p->eType = eType;
  p->rNum = eType==SQLITE_NULL ? 0.0 : sqlite3_value_double(pVal);
  return SQLITE_OK;
}

static void columnarValueClear(ColumnarValue *p){
  if( p->eType==SQLITE_TEXT || p->eType==SQLITE_BLOB ){
    sqlite3_free(p->u.p);
  }
  p->u.p = 0;
  p->nVal = 0;
  p->eType = 0;
}

static void columnarValueResult(sqlite3_context *ctx, const ColumnarValue *p){
  switch( p->eType ){
    case SQLITE_INTEGER:
      sqlite3_result_int64(ctx, p->u.i);
      break;
    case SQLITE_FLOAT:
      sqlite3_result_double(ctx, p->u.r);
      break;
    case SQLITE_TEXT:
      /* A null pointer would make this a NULL result rather than an empty
      ** string, so hand sqlite3_result_text() something valid for nVal==0. */
      sqlite3_result_text(ctx, p->nVal ? (const char*)p->u.p : "", p->nVal, SQLITE_TRANSIENT);
      break;
    case SQLITE_BLOB:
      sqlite3_result_blob(ctx, p->nVal ? p->u.p : "", p->nVal, SQLITE_TRANSIENT);
      break;
    default:
      sqlite3_result_null(ctx);
      break;
  }
}

/* Sort class, following SQLite's NULL < numeric < text < blob ordering. */
static int columnarValueClass(int eType){
  switch( eType ){
    case SQLITE_NULL:     return 0;
    case SQLITE_INTEGER:
    case SQLITE_FLOAT:    return 1;
    case SQLITE_TEXT:     return 2;
    default:              return 3;
  }
}

/* Compare a stored value against pVal in SQLite's BINARY sort order, returning
** a negative, zero or positive result as *p sorts before, with, or after
** pVal. */
static int columnarValueCompare(const ColumnarValue *p, sqlite3_value *pVal){
  int eType = sqlite3_value_type(pVal);
  int cA = columnarValueClass(p->eType);
  int cB = columnarValueClass(eType);

  if( cA!=cB ) return cA<cB ? -1 : 1;
  if( cA==0 ) return 0;
  if( cA==1 ){
    if( p->eType==SQLITE_INTEGER && eType==SQLITE_INTEGER ){
      sqlite3_int64 a = p->u.i;
      sqlite3_int64 b = sqlite3_value_int64(pVal);
      return a<b ? -1 : (a>b ? 1 : 0);
    }else{
      double a = p->eType==SQLITE_INTEGER ? (double)p->u.i : p->u.r;
      double b = sqlite3_value_double(pVal);
      return a<b ? -1 : (a>b ? 1 : 0);
    }
  }
  {
    const void *aB;
    int nB;
    int n;
    int c;
    if( cA==2 ){
      aB = (const void*)sqlite3_value_text(pVal);
    }else{
      aB = sqlite3_value_blob(pVal);
    }
    nB = sqlite3_value_bytes(pVal);
    if( aB==0 ) nB = 0;
    n = p->nVal<nB ? p->nVal : nB;
    c = n>0 ? memcmp(p->u.p, aB, (size_t)n) : 0;
    if( c!=0 ) return c<0 ? -1 : 1;
    return p->nVal<nB ? -1 : (p->nVal>nB ? 1 : 0);
  }
}

/* Grouping-key equality.  Unlike columnarValueCompare() this treats an integer
** and a float holding the same value as one key, matching what GROUP BY does.
*/
static int columnarValueEqual(const ColumnarValue *p, sqlite3_value *pVal){
  int eType = sqlite3_value_type(pVal);
  if( p->eType==SQLITE_NULL || eType==SQLITE_NULL ){
    return p->eType==SQLITE_NULL && eType==SQLITE_NULL;
  }
  if( p->eType==SQLITE_INTEGER && eType==SQLITE_INTEGER ){
    return p->u.i==sqlite3_value_int64(pVal);
  }
  if( p->eType==SQLITE_INTEGER && eType==SQLITE_FLOAT ){
    sqlite3_int64 iVal;
    return columnarDoubleToInt64(sqlite3_value_double(pVal), &iVal)
        && p->u.i==iVal;
  }
  if( p->eType==SQLITE_FLOAT && eType==SQLITE_INTEGER ){
    sqlite3_int64 iVal;
    return columnarDoubleToInt64(p->u.r, &iVal)
        && iVal==sqlite3_value_int64(pVal);
  }
  if( p->eType==SQLITE_FLOAT && eType==SQLITE_FLOAT ){
    return p->u.r==sqlite3_value_double(pVal);
  }
  if( p->eType!=eType ) return 0;
  return columnarValueCompare(p, pVal)==0;
}

// PRAGMA MARK: - Running Sums -

static void columnarSumInit(ColumnarSum *p){
  p->nVal = 0;
  p->iSum = 0;
  p->rSum = 0.0;
  p->rErr = 0.0;
  p->bInt = 1;
}

static double columnarFabs(double r){
  return r<0.0 ? -r : r;
}

/* One Kahan-Babuska-Neumaier step, the compensated summation SQLite's own
** sum() and avg() use.  Without it the floating point result drifts from
** theirs in the last digits. */
static void columnarSumStep(ColumnarSum *p, double r){
  double s = p->rSum;
  double t = s + r;
  if( columnarFabs(s) > columnarFabs(r) ){
    p->rErr += (s - t) + r;
  }else{
    p->rErr += (r - t) + s;
  }
  p->rSum = t;
}

/* Fold in a sum of nVal integer values. */
static void columnarSumAddInt(ColumnarSum *p, sqlite3_int64 iVal, sqlite3_int64 nVal){
  p->nVal += nVal;
  /* Large magnitudes are split so the compensated sum keeps every bit, the
  ** way SQLite splits them. */
  if( iVal<=-4503599627370496LL || iVal>=4503599627370496LL ){
    sqlite3_int64 iSml = iVal % 16384;
    columnarSumStep(p, (double)(iVal - iSml));
    columnarSumStep(p, (double)iSml);
  }else{
    columnarSumStep(p, (double)iVal);
  }
  if( p->bInt ){
    if( (iVal>0 && p->iSum>COLUMNAR_MAX_I64-iVal)
     || (iVal<0 && p->iSum<COLUMNAR_MIN_I64-iVal)
    ){
      /* The exact sum no longer fits; fall back to the compensated double
      ** rather than failing the query the way sum() would. */
      p->bInt = 0;
    }else{
      p->iSum += iVal;
    }
  }
}

/* Fold in a sum of nVal values that is not exactly an integer. */
static void columnarSumAddDouble(ColumnarSum *p, double rVal, sqlite3_int64 nVal){
  p->nVal += nVal;
  columnarSumStep(p, rVal);
  p->bInt = 0;
}

static void columnarSumAddValue(ColumnarSum *p, sqlite3_value *pVal){
  switch( sqlite3_value_type(pVal) ){
    case SQLITE_NULL:
      break;
    case SQLITE_INTEGER:
      columnarSumAddInt(p, sqlite3_value_int64(pVal), 1);
      break;
    case SQLITE_FLOAT:
      columnarSumAddDouble(p, sqlite3_value_double(pVal), 1);
      break;
    default:
      /* Text or a blob.  sum() applies numeric affinity before accumulating,
      ** so text holding a plain integer would keep an exact accumulator;
      ** finding that out here needs a numeric-affinity conversion of a copy,
      ** and that combination is not safe across SQLite versions, so take the
      ** floating point path.  The total is the same either way -- only its
      ** storage class differs, and only for numeric text in a column with no
      ** declared affinity, since a numeric column converts such text as it is
      ** stored. */
      columnarSumAddDouble(p, sqlite3_value_double(pVal), 1);
      break;
  }
}

static void columnarSumAddColumn(ColumnarSum *p, sqlite3_stmt *pStmt, int iCol){
  columnarSumAddValue(p, sqlite3_column_value(pStmt, iCol));
}

/* The compensated total.  A NaN correction term is dropped, as SQLite does. */
static double columnarSumDouble(const ColumnarSum *p){
  double r = p->rSum;
  if( p->rErr==p->rErr ) r += p->rErr;
  return r;
}

static void columnarSumResult(sqlite3_context *ctx, const ColumnarSum *p){
  if( p->nVal==0 ){
    sqlite3_result_null(ctx);
  }else if( p->bInt ){
    sqlite3_result_int64(ctx, p->iSum);
  }else{
    sqlite3_result_double(ctx, columnarSumDouble(p));
  }
}

static void columnarAvgResult(sqlite3_context *ctx, const ColumnarSum *p){
  if( p->nVal==0 ){
    sqlite3_result_null(ctx);
  }else if( p->bInt ){
    sqlite3_result_double(ctx, (double)p->iSum/(double)p->nVal);
  }else{
    sqlite3_result_double(ctx, columnarSumDouble(p)/(double)p->nVal);
  }
}

// PRAGMA MARK: - Column Affinity -

#define COLUMNAR_AFF_NONE  0       /* BLOB affinity: compare values as stored */
#define COLUMNAR_AFF_TEXT  1
#define COLUMNAR_AFF_NUM   2       /* INTEGER, REAL or NUMERIC */

/* Comparison affinity of a declared type, by SQLite's rules. */
static int columnarAffinityOfType(const char *zType){
  int n = zType ? (int)strlen(zType) : 0;
  if( n==0 ) return COLUMNAR_AFF_NONE;
  if( columnarTypeContains(zType, n, "INT") ) return COLUMNAR_AFF_NUM;
  if( columnarTypeContains(zType, n, "CHAR")
   || columnarTypeContains(zType, n, "CLOB")
   || columnarTypeContains(zType, n, "TEXT")
  ){
    return COLUMNAR_AFF_TEXT;
  }
  if( columnarTypeContains(zType, n, "BLOB") ) return COLUMNAR_AFF_NONE;
  if( columnarTypeContains(zType, n, "REAL")
   || columnarTypeContains(zType, n, "FLOA")
   || columnarTypeContains(zType, n, "DOUB")
  ){
    return COLUMNAR_AFF_NUM;
  }
  return COLUMNAR_AFF_NUM;         /* NUMERIC */
}

/* Read a columnar column's affinity back out of its shadow table schema.
** Databases written before shadow columns carried an affinity report no type,
** which yields COLUMNAR_AFF_NONE -- the right answer for their untyped data.
*/
static int columnarColumnAffinity(sqlite3 *db, const ColumnarTableRef *pRef, int iCol, int *peAff){
  sqlite3_stmt *pStmt = 0;
  char *zSql;
  int rc;

  *peAff = COLUMNAR_AFF_NONE;
  zSql = sqlite3_mprintf("PRAGMA \"%w\".table_info(\"%w__columnar_c%d\")", pRef->zDb, pRef->zTab, iCol);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ) return rc;
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    const char *zName = (const char*)sqlite3_column_text(pStmt, 1);
    if( zName && sqlite3_stricmp(zName, "v")==0 ){
      *peAff = columnarAffinityOfType((const char*)sqlite3_column_text(pStmt, 2));
      break;
    }
  }
  if( rc==SQLITE_ROW || rc==SQLITE_DONE ) rc = SQLITE_OK;
  sqlite3_finalize(pStmt);
  return rc;
}

/* True if a bound can be compared against the zone map exactly as it stands.
**
** A comparison against the real column applies that column's affinity to the
** bound first.  The zone map's min/max columns are typeless, so they cannot
** express that conversion, and converting the bound here is not an option: it
** would mean reimplementing SQLite's own numeric-affinity rules, and doing it
** by duplicating the value and converting the copy is not safe across SQLite
** versions -- that combination crashes on some builds, and a loadable
** extension runs against whatever version the host provides.
**
** So when a bound would need converting, report the zone map as unusable and
** let the caller scan instead.  The scan compares against the shadow column,
** where SQLite applies the affinity itself, so the answer stays correct; only
** the chunk-skipping optimization is given up, and only for a bound whose
** type does not match the column it filters on.
*/
static int columnarZoneBoundUsable(int eAff, sqlite3_value *pVal){
  int eType = sqlite3_value_type(pVal);
  if( eAff==COLUMNAR_AFF_NONE || eType==SQLITE_NULL ) return 1;
  if( eAff==COLUMNAR_AFF_NUM ){
    return eType==SQLITE_INTEGER || eType==SQLITE_FLOAT;
  }
  return eType==SQLITE_TEXT || eType==SQLITE_BLOB;
}

static int columnarZoneMapUsable(sqlite3 *db, const ColumnarTableRef *pRef, int iFilterCol, sqlite3_value *pLo, sqlite3_value *pHi, int *pbUsable){
  int eAff = COLUMNAR_AFF_NONE;
  int rc;

  *pbUsable = 0;
  rc = columnarColumnAffinity(db, pRef, iFilterCol, &eAff);
  if( rc!=SQLITE_OK ) return rc;
  *pbUsable = columnarZoneBoundUsable(eAff, pLo)
           && columnarZoneBoundUsable(eAff, pHi);
  return SQLITE_OK;
}

// PRAGMA MARK: - Metadata -

static int columnarEnsureMetaTable(sqlite3 *db, const ColumnarTableRef *pRef, char **pzErr){
  char *zSql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\".\"%w__columnar_meta\"(" "k TEXT PRIMARY KEY," "v INTEGER NOT NULL" ")", pRef->zDb, pRef->zTab);
  int rc;
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = columnarExec(db, pzErr, zSql);
  sqlite3_free(zSql);
  return rc;
}

/* True if zTab names an existing table in schema zDb.  The comparison is
** NOCASE because table names are, so that columnar_analyze('MYTAB') still
** finds the shadow tables of a table created as MyTab. */
static int columnarTableExists(sqlite3 *db, const char *zDb, const char *zTab, int *pbExists){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(db, &pStmt, 0, "SELECT 1 FROM \"%w\".sqlite_master WHERE type='table' AND name=?1 COLLATE NOCASE", zDb);
  *pbExists = 0;
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_text(pStmt, 1, zTab, -1, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    *pbExists = 1;
    rc = SQLITE_OK;
  }else if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
  }
  sqlite3_finalize(pStmt);
  return rc;
}

/* Confirm pRef really is a columnar table.  columnar_analyze() creates missing
** shadow tables on demand, so without this check an unrelated -- or simply
** misspelled -- table name would leave a set of orphan shadow tables behind in
** the schema before the first real error was reported. */
static int columnarRequireColumnarTable(sqlite3 *db, const ColumnarTableRef *pRef, char **pzErr){
  char *zRowTab = sqlite3_mprintf("%s__columnar_rowid", pRef->zTab);
  int bExists = 0;
  int rc;
  if( zRowTab==0 ) return SQLITE_NOMEM;
  rc = columnarTableExists(db, pRef->zDb, zRowTab, &bExists);
  sqlite3_free(zRowTab);
  if( rc!=SQLITE_OK ) return rc;
  if( !bExists ){
    if( pzErr ){
      *pzErr = sqlite3_mprintf("no such columnar table: %s", pRef->zTab);
    }
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

static int columnarMetaGetInt(sqlite3 *db, const ColumnarTableRef *pRef, const char *zKey, sqlite3_int64 *pVal, int *pbFound){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(db, &pStmt, 0, "SELECT v FROM \"%w\".\"%w__columnar_meta\" WHERE k=?1", pRef->zDb, pRef->zTab);
  *pVal = 0;
  *pbFound = 0;
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_text(pStmt, 1, zKey, -1, SQLITE_STATIC);
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    *pVal = sqlite3_column_int64(pStmt, 0);
    *pbFound = 1;
    rc = SQLITE_OK;
  }else if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
  }
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarMetaSetInt(sqlite3 *db, const ColumnarTableRef *pRef, const char *zKey, sqlite3_int64 iVal){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(db, &pStmt, 0, "REPLACE INTO \"%w\".\"%w__columnar_meta\"(k,v) VALUES(?1,?2)", pRef->zDb, pRef->zTab);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_text(pStmt, 1, zKey, -1, SQLITE_STATIC);
  sqlite3_bind_int64(pStmt, 2, iVal);
  rc = columnarStepDone(pStmt);
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarMetaIncrInt(sqlite3 *db, const ColumnarTableRef *pRef, const char *zKey, sqlite3_int64 iDelta){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(db, &pStmt, 0, "INSERT OR IGNORE INTO \"%w\".\"%w__columnar_meta\"(k,v)" " VALUES(?1,0)", pRef->zDb, pRef->zTab);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_text(pStmt, 1, zKey, -1, SQLITE_STATIC);
  rc = columnarStepDone(pStmt);
  sqlite3_finalize(pStmt);
  if( rc!=SQLITE_OK ) return rc;

  rc = columnarPrepare(db, &pStmt, 0, "UPDATE \"%w\".\"%w__columnar_meta\" SET v=v+?1 WHERE k=?2", pRef->zDb, pRef->zTab);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_int64(pStmt, 1, iDelta);
  sqlite3_bind_text(pStmt, 2, zKey, -1, SQLITE_STATIC);
  rc = columnarStepDone(pStmt);
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarMetaEnsureBase(sqlite3 *db, const ColumnarTableRef *pRef, int nCol){
  int rc;
  rc = columnarMetaSetInt(db, pRef, "format_version", 1);
  if( rc==SQLITE_OK ){
    rc = columnarMetaSetInt(db, pRef, "chunk_size", COLUMNAR_CHUNK_SIZE);
  }
  if( rc==SQLITE_OK && nCol>=0 ){
    rc = columnarMetaSetInt(db, pRef, "ncol", nCol);
  }
  return rc;
}

static sqlite3_int64 columnarChunkOfRowid(sqlite3_int64 iRowid){
  return (iRowid - 1) / COLUMNAR_CHUNK_SIZE;
}

static int columnarEnsureDirtyTable(sqlite3 *db, const ColumnarTableRef *pRef, char **pzErr){
  char *zSql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\".\"%w__columnar_dirty\"(" "col INTEGER NOT NULL," "chunk INTEGER NOT NULL," "PRIMARY KEY(col,chunk)" ")", pRef->zDb, pRef->zTab);
  int rc;
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = columnarExec(db, pzErr, zSql);
  sqlite3_free(zSql);
  return rc;
}

static int columnarMarkDirtyRowid(ColumnarVtab *p, sqlite3_int64 iRowid){
  ColumnarTableRef sRef = {p->zDb, p->zName};
  sqlite3_stmt *pStmt = 0;
  sqlite3_int64 iChunk = columnarChunkOfRowid(iRowid);
  sqlite3_int64 nNewDirty = 0;
  int rc;
  int i;

  rc = columnarEnsureMetaTable(p->db, &sRef, 0);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarMetaEnsureBase(p->db, &sRef, p->nCol);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarMetaSetInt(p->db, &sRef, "stats_valid", 0);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarEnsureDirtyTable(p->db, &sRef, 0);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarPrepare(p->db, &pStmt, 0, "DELETE FROM %Q.%Q", p->zDb, p->zStatTab);
  if( rc==SQLITE_OK ) rc = columnarStepDone(pStmt);
  sqlite3_finalize(pStmt);
  pStmt = 0;
  if( rc!=SQLITE_OK ) return rc;

  rc = columnarPrepare(p->db, &pStmt, 0, "INSERT OR IGNORE INTO %Q.%Q(col,chunk) VALUES(?1,?2)", p->zDb, p->zDirtyTab);
  if( rc!=SQLITE_OK ) return rc;
  for(i=0; rc==SQLITE_OK && i<p->nCol; i++){
    sqlite3_bind_int(pStmt, 1, i);
    sqlite3_bind_int64(pStmt, 2, iChunk);
    rc = columnarStepDone(pStmt);
    if( rc==SQLITE_OK ) nNewDirty += sqlite3_changes(p->db);
    sqlite3_reset(pStmt);
    sqlite3_clear_bindings(pStmt);
  }
  sqlite3_finalize(pStmt);
  if( rc==SQLITE_OK && nNewDirty>0 ){
    rc = columnarMetaIncrInt(p->db, &sRef, "dirty_count", nNewDirty);
  }
  return rc;
}

// PRAGMA MARK: - Row Mutation -

static int columnarDeleteRow(ColumnarVtab *p, sqlite3_int64 iRowid){
  sqlite3_stmt *pStmt = 0;
  int rc;
  int i;
  rc = columnarPrepare(p->db, &pStmt, 0, "DELETE FROM %Q.%Q WHERE rid=?1", p->zDb, p->zRowTab);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iRowid);
    rc = columnarStepDone(pStmt);
  }
  sqlite3_finalize(pStmt);
  for(i=0; rc==SQLITE_OK && i<p->nCol; i++){
    rc = columnarPrepare(p->db, &pStmt, 0, "DELETE FROM %Q.%Q WHERE rid=?1", p->zDb, p->azColTab[i]);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pStmt, 1, iRowid);
      rc = columnarStepDone(pStmt);
    }
    sqlite3_finalize(pStmt);
  }
  return rc;
}

static int columnarNextRowid(ColumnarVtab *p, sqlite3_int64 *pRowid){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(p->db, &pStmt, 0, "SELECT coalesce(max(rid),0)+1 FROM %Q.%Q", p->zDb, p->zRowTab);
  if( rc==SQLITE_OK ){
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      *pRowid = sqlite3_column_int64(pStmt, 0);
      rc = SQLITE_OK;
    }
  }
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarRowidExists(ColumnarVtab *p, sqlite3_int64 iRowid, int *pbExists){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(p->db, &pStmt, 0, "SELECT 1 FROM %Q.%Q WHERE rid=?1", p->zDb, p->zRowTab);
  *pbExists = 0;
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iRowid);
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      *pbExists = 1;
      rc = SQLITE_OK;
    }else if( rc==SQLITE_DONE ){
      rc = SQLITE_OK;
    }
  }
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarInsertRow(ColumnarVtab *p, sqlite3_int64 iRowid, int argc, sqlite3_value **argv){
  sqlite3_stmt *pStmt = 0;
  /* xUpdate passes two leading values plus one per declared column.  p->nCol
  ** should equal argc-2, but clamp rather than trust it: reading past argv[]
  ** would hand sqlite3_bind_value() a wild pointer. */
  int nCol = argc-2 < p->nCol ? argc-2 : p->nCol;
  int rc;
  int i;
  const char *zConflict = "";
  int eConflict = sqlite3_vtab_on_conflict(p->db);
  if( eConflict==SQLITE_REPLACE ) zConflict = "OR REPLACE ";
  else if( eConflict==SQLITE_IGNORE ) zConflict = "OR IGNORE ";
  else if( eConflict==SQLITE_FAIL ) zConflict = "OR FAIL ";
  else if( eConflict==SQLITE_ROLLBACK ) zConflict = "OR ROLLBACK ";

  rc = columnarPrepare(p->db, &pStmt, 0, "INSERT %sINTO %Q.%Q(rid) VALUES(?1)", zConflict, p->zDb, p->zRowTab);
  if( rc==SQLITE_OK ){
    sqlite3_bind_int64(pStmt, 1, iRowid);
    rc = columnarStepDone(pStmt);
  }
  sqlite3_finalize(pStmt);
  for(i=0; rc==SQLITE_OK && i<nCol; i++){
    rc = columnarPrepare(p->db, &pStmt, 0, "INSERT %sINTO %Q.%Q(rid,v) VALUES(?1,?2)", zConflict, p->zDb, p->azColTab[i]);
    if( rc==SQLITE_OK ){
      sqlite3_bind_int64(pStmt, 1, iRowid);
      sqlite3_bind_value(pStmt, 2, argv[2+i]);
      rc = columnarStepDone(pStmt);
    }
    sqlite3_finalize(pStmt);
  }
  return rc;
}

static int columnarUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv, sqlite_int64 *pRowid){
  ColumnarVtab *p = (ColumnarVtab*)pVtab;
  ColumnarTableRef sRef = {p->zDb, p->zName};
  sqlite3_int64 oldRowid = 0;
  sqlite3_int64 newRowid = 0;
  int bExists = 0;
  int bInsert = 0;
  int bReplaceExisting = 0;
  int rc;

  if( argc==1 ){
    oldRowid = sqlite3_value_int64(argv[0]);
    rc = columnarRowidExists(p, oldRowid, &bExists);
    if( rc!=SQLITE_OK ) return rc;
    rc = columnarMarkDirtyRowid(p, oldRowid);
    if( rc!=SQLITE_OK ) return rc;
    rc = columnarDeleteRow(p, oldRowid);
    if( rc==SQLITE_OK && bExists ){
      rc = columnarMetaIncrInt(p->db, &sRef, "row_count", -1);
    }
    return rc;
  }

  if( sqlite3_value_type(argv[1])==SQLITE_NULL ){
    rc = columnarNextRowid(p, &newRowid);
    if( rc!=SQLITE_OK ) return rc;
  }else{
    newRowid = sqlite3_value_int64(argv[1]);
  }

  bInsert = sqlite3_value_type(argv[0])==SQLITE_NULL;
  if( !bInsert ){
    oldRowid = sqlite3_value_int64(argv[0]);
    if( newRowid!=oldRowid ){
      rc = columnarRowidExists(p, newRowid, &bReplaceExisting);
      if( rc!=SQLITE_OK ) return rc;
    }
    rc = columnarMarkDirtyRowid(p, oldRowid);
    if( rc!=SQLITE_OK ) return rc;
    rc = columnarDeleteRow(p, oldRowid);
    if( rc!=SQLITE_OK ) return rc;
  }

  if( bInsert ){
    rc = columnarRowidExists(p, newRowid, &bExists);
    if( rc!=SQLITE_OK ) return rc;
  }
  rc = columnarMarkDirtyRowid(p, newRowid);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarInsertRow(p, newRowid, argc, argv);
  if( rc==SQLITE_OK ){
    if( bInsert && !bExists ){
      rc = columnarMetaIncrInt(p->db, &sRef, "row_count", 1);
    }else if( !bInsert && bReplaceExisting ){
      rc = columnarMetaIncrInt(p->db, &sRef, "row_count", -1);
    }
  }
  if( rc==SQLITE_OK && pRowid ) *pRowid = newRowid;
  return rc;
}

static int columnarShadowName(const char *zName){
  const char *z = zName;
  const char *zMark = 0;
  while( z && (z = strstr(z, "__columnar_"))!=0 ){
    zMark = z;
    z++;
  }
  if( zMark==0 ) return 0;
  z = zMark + 11;
  if( strcmp(z, "rowid")==0 ) return 1;
  if( strcmp(z, "stat")==0 ) return 1;
  if( strcmp(z, "chunk")==0 ) return 1;
  if( strcmp(z, "dirty")==0 ) return 1;
  if( strcmp(z, "meta")==0 ) return 1;
  if( z[0]!='c' || z[1]==0 ) return 0;
  z++;
  while( z[0]>='0' && z[0]<='9' ) z++;
  return z[0]==0;
}

static const char *columnarValueText(sqlite3_value *pVal){
  return (const char*)sqlite3_value_text(pVal);
}

// PRAGMA MARK: - Table References and Arguments -

static void columnarClearTableRef(ColumnarTableRef *pRef){
  sqlite3_free(pRef->zDb);
  sqlite3_free(pRef->zTab);
  pRef->zDb = 0;
  pRef->zTab = 0;
}

static int columnarParseTableRef(const char *zRef, ColumnarTableRef *pRef, char **pzErr){
  const char *zDot;
  int nDb;

  pRef->zDb = 0;
  pRef->zTab = 0;
  if( zRef==0 || zRef[0]==0 ){
    if( pzErr ) *pzErr = sqlite3_mprintf("invalid table reference");
    return SQLITE_ERROR;
  }
  zDot = strchr(zRef, '.');
  if( zDot==0 ){
    pRef->zDb = sqlite3_mprintf("main");
    pRef->zTab = sqlite3_mprintf("%s", zRef);
  }else{
    if( zDot==zRef || zDot[1]==0 || strchr(zDot+1, '.')!=0 ){
      if( pzErr ){
        *pzErr = sqlite3_mprintf(
            "invalid table reference: expected table or db.table");
      }
      return SQLITE_ERROR;
    }
    nDb = (int)(zDot - zRef);
    pRef->zDb = sqlite3_mprintf("%.*s", nDb, zRef);
    pRef->zTab = sqlite3_mprintf("%s", zDot+1);
  }
  if( pRef->zDb==0 || pRef->zTab==0 ){
    columnarClearTableRef(pRef);
    return SQLITE_NOMEM;
  }
  return SQLITE_OK;
}

static int columnarParseTableArg(sqlite3_context *ctx, sqlite3_value *pVal, ColumnarTableRef *pRef){
  char *zErr = 0;
  int rc = columnarParseTableRef(columnarValueText(pVal), pRef, &zErr);
  if( rc!=SQLITE_OK ){
    if( rc==SQLITE_NOMEM ){
      sqlite3_result_error_nomem(ctx);
    }else{
      sqlite3_result_error(ctx, zErr ? zErr : "invalid table reference", -1);
    }
    sqlite3_free(zErr);
  }
  return rc;
}

static int columnarResolveColumn(sqlite3 *db, const ColumnarTableRef *pRef, sqlite3_value *pCol, int *piCol, char **pzErr){
  sqlite3_stmt *pStmt = 0;
  char *zSql;
  int rc;
  int iCol = -1;

  if( sqlite3_value_type(pCol)==SQLITE_INTEGER ){
    iCol = sqlite3_value_int(pCol);
    if( iCol<0 ){
      *pzErr = sqlite3_mprintf("column index must be non-negative");
      return SQLITE_ERROR;
    }
    *piCol = iCol;
    return SQLITE_OK;
  }

  zSql = sqlite3_mprintf("PRAGMA \"%w\".table_xinfo(%Q)", pRef->zDb, pRef->zTab);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ){
    *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    return rc;
  }

  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    const char *zName = (const char*)sqlite3_column_text(pStmt, 1);
    if( zName && sqlite3_stricmp(zName, columnarValueText(pCol))==0 ){
      iCol = sqlite3_column_int(pStmt, 0);
      break;
    }
  }
  if( rc==SQLITE_ROW ) rc = SQLITE_OK;
  if( rc==SQLITE_DONE ) rc = SQLITE_OK;
  sqlite3_finalize(pStmt);
  if( rc!=SQLITE_OK ){
    *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    return rc;
  }
  if( iCol<0 ){
    *pzErr = sqlite3_mprintf("no such column: %s.%s",
        pRef->zTab, columnarValueText(pCol));
    return SQLITE_ERROR;
  }
  *piCol = iCol;
  return SQLITE_OK;
}

static int columnarColumnCount(sqlite3 *db, const ColumnarTableRef *pRef, int *pnCol, char **pzErr){
  sqlite3_stmt *pStmt = 0;
  char *zSql;
  int rc;
  int nCol = 0;

  zSql = sqlite3_mprintf("PRAGMA \"%w\".table_xinfo(%Q)", pRef->zDb, pRef->zTab);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ){
    if( pzErr ) *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    return rc;
  }
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    nCol++;
  }
  if( rc==SQLITE_DONE ) rc = SQLITE_OK;
  sqlite3_finalize(pStmt);
  if( rc!=SQLITE_OK ){
    if( pzErr ) *pzErr = sqlite3_mprintf("%s", sqlite3_errmsg(db));
    return rc;
  }
  if( nCol==0 ){
    if( pzErr ){
      *pzErr = sqlite3_mprintf("no such columnar table: %s", pRef->zTab);
    }
    return SQLITE_ERROR;
  }
  *pnCol = nCol;
  return SQLITE_OK;
}

static int columnarResolveColumns(sqlite3 *db, const ColumnarTableRef *pRef, sqlite3_value **argv, int nCol, int *aCol, char **pzErr){
  int i;
  for(i=0; i<nCol; i++){
    int rc = columnarResolveColumn(db, pRef, argv[i], &aCol[i], pzErr);
    if( rc!=SQLITE_OK ) return rc;
  }
  return SQLITE_OK;
}

static int columnarParseTableColumnsArgs(sqlite3_context *ctx, int argc, sqlite3_value **argv, ColumnarTableRef *pRef, int nCol, int nExtra, int *aCol){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  char *zErr = 0;
  int rc;

  if( argc!=1+nCol+nExtra ){
    sqlite3_result_error(ctx, "wrong number of arguments", -1);
    return SQLITE_ERROR;
  }
  rc = columnarParseTableArg(ctx, argv[0], pRef);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarResolveColumns(db, pRef, &argv[1], nCol, aCol, &zErr);
  if( rc!=SQLITE_OK ) goto table_columns_args_error;
  if( pRef->zDb==0 || pRef->zTab==0 ){
    sqlite3_result_error(ctx, "invalid columnar arguments", -1);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;

table_columns_args_error:
  sqlite3_result_error(ctx, zErr ? zErr : sqlite3_errmsg(db), -1);
  sqlite3_free(zErr);
  columnarClearTableRef(pRef);
  return rc;
}

static int columnarParseVtabTableColumns(sqlite3 *db, sqlite3_value **argv, ColumnarTableRef *pRef, int nCol, int *aCol, char **pzErr){
  int rc = columnarParseTableRef(columnarValueText(argv[0]), pRef, pzErr);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarResolveColumns(db, pRef, &argv[1], nCol, aCol, pzErr);
  if( rc!=SQLITE_OK ) columnarClearTableRef(pRef);
  return rc;
}

// PRAGMA MARK: - Scalar Analytics -

static int columnarAnalyticFromStats(sqlite3_context *ctx, const ColumnarTableRef *pRef, int iCol, int eOp, int *pbDone){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  sqlite3_stmt *pStmt = 0;
  char *zSql;
  int rc;

  *pbDone = 0;
  if( eOp==COLUMNAR_ANALYTIC_COUNT && iCol<0 ){
    zSql = sqlite3_mprintf("SELECT row_count FROM \"%w\".\"%w__columnar_stat\" LIMIT 1", pRef->zDb, pRef->zTab);
  }else{
    zSql = sqlite3_mprintf("SELECT nonnull_count,sum FROM \"%w\".\"%w__columnar_stat\"" " WHERE col=?1", pRef->zDb, pRef->zTab);
  }
  if( zSql==0 ){
    sqlite3_result_error_nomem(ctx);
    return SQLITE_NOMEM;
  }
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ){
    /* Databases created by older versions might not have stats yet. */
    return SQLITE_OK;
  }
  if( !(eOp==COLUMNAR_ANALYTIC_COUNT && iCol<0) ){
    sqlite3_bind_int(pStmt, 1, iCol);
  }
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    sqlite3_int64 n;
    *pbDone = 1;
    if( eOp==COLUMNAR_ANALYTIC_COUNT && iCol<0 ){
      sqlite3_result_int64(ctx, sqlite3_column_int64(pStmt, 0));
    }else{
      n = sqlite3_column_int64(pStmt, 0);
      if( eOp==COLUMNAR_ANALYTIC_COUNT ){
        sqlite3_result_int64(ctx, n);
      }else if( n==0 ){
        sqlite3_result_null(ctx);
      }else if( eOp==COLUMNAR_ANALYTIC_SUM ){
        sqlite3_result_value(ctx, sqlite3_column_value(pStmt, 1));
      }else{
        assert( eOp==COLUMNAR_ANALYTIC_AVG );
        sqlite3_result_double(ctx, sqlite3_column_double(pStmt, 1)/(double)n);
      }
    }
    rc = SQLITE_OK;
  }else if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
  }
  sqlite3_finalize(pStmt);
  return rc;
}

static void columnarAnalyticFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const ColumnarAnalytic *pAnalytic =
      (const ColumnarAnalytic*)sqlite3_user_data(ctx);
  int eOp = pAnalytic->eOp;
  ColumnarTableRef sRef = {0, 0};
  sqlite3_stmt *pStmt = 0;
  char *zSql = 0;
  int iCol = -1;
  int aCol[1];
  int rc;
  ColumnarSum sAcc;
  int bDone = 0;

  columnarSumInit(&sAcc);
  rc = columnarParseTableColumnsArgs(ctx, argc, argv, &sRef, eOp!=COLUMNAR_ANALYTIC_COUNT || argc>1, 0, aCol);
  if( rc!=SQLITE_OK ) return;
  if( eOp!=COLUMNAR_ANALYTIC_COUNT || argc>1 ) iCol = aCol[0];

  rc = columnarAnalyticFromStats(ctx, &sRef, iCol, eOp, &bDone);
  if( rc!=SQLITE_OK || bDone ){
    columnarClearTableRef(&sRef);
    return;
  }

  if( eOp==COLUMNAR_ANALYTIC_COUNT && iCol<0 ){
    zSql = sqlite3_mprintf("SELECT rid FROM \"%w\".\"%w__columnar_rowid\"", sRef.zDb, sRef.zTab);
  }else{
    zSql = sqlite3_mprintf("SELECT v FROM \"%w\".\"%w__columnar_c%d\"", sRef.zDb, sRef.zTab, iCol);
  }
  if( zSql==0 ){
    sqlite3_result_error_nomem(ctx);
    columnarClearTableRef(&sRef);
    return;
  }
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    columnarClearTableRef(&sRef);
    return;
  }

  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    if( eOp==COLUMNAR_ANALYTIC_COUNT && iCol<0 ){
      sAcc.nVal++;
    }else if( sqlite3_column_type(pStmt, 0)!=SQLITE_NULL ){
      if( eOp!=COLUMNAR_ANALYTIC_COUNT ){
        columnarSumAddColumn(&sAcc, pStmt, 0);
      }else{
        sAcc.nVal++;
      }
    }
  }
  if( rc!=SQLITE_DONE ){
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    sqlite3_finalize(pStmt);
    columnarClearTableRef(&sRef);
    return;
  }
  sqlite3_finalize(pStmt);
  columnarClearTableRef(&sRef);

  switch( eOp ){
    case COLUMNAR_ANALYTIC_SUM:
      columnarSumResult(ctx, &sAcc);
      break;
    case COLUMNAR_ANALYTIC_AVG:
      columnarAvgResult(ctx, &sAcc);
      break;
    default:
      assert( eOp==COLUMNAR_ANALYTIC_COUNT );
      sqlite3_result_int64(ctx, sAcc.nVal);
      break;
  }
}

static int columnarInt64Query(sqlite3 *db, sqlite3_int64 *pVal, const char *zFmt, ...){
  va_list ap;
  char *zSql;
  sqlite3_stmt *pStmt = 0;
  int rc;

  va_start(ap, zFmt);
  zSql = sqlite3_vmprintf(zFmt, ap);
  va_end(ap);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc==SQLITE_OK ){
    rc = sqlite3_step(pStmt);
    if( rc==SQLITE_ROW ){
      *pVal = sqlite3_column_int64(pStmt, 0);
      rc = SQLITE_OK;
    }
  }
  sqlite3_finalize(pStmt);
  return rc;
}

// PRAGMA MARK: - Analyze -

static int columnarEnsureChunkTable(sqlite3 *db, const ColumnarTableRef *pRef, char **pzErr){
  char *zSql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\".\"%w__columnar_chunk\"(" "col INTEGER NOT NULL," "chunk INTEGER NOT NULL," "rid_min INTEGER NOT NULL," "rid_max INTEGER NOT NULL," "row_count INTEGER NOT NULL," "nonnull_count INTEGER NOT NULL," "sum," "min," "max," "PRIMARY KEY(col,chunk)" ")", pRef->zDb, pRef->zTab);
  int rc;
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = columnarExec(db, pzErr, zSql);
  sqlite3_free(zSql);
  return rc;
}

static int columnarEnsureStatTable(sqlite3 *db, const ColumnarTableRef *pRef, char **pzErr){
  char *zSql = sqlite3_mprintf("CREATE TABLE IF NOT EXISTS \"%w\".\"%w__columnar_stat\"(" "col INTEGER PRIMARY KEY," "row_count INTEGER NOT NULL," "nonnull_count INTEGER NOT NULL," "sum," "min," "max" ")", pRef->zDb, pRef->zTab);
  int rc;
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = columnarExec(db, pzErr, zSql);
  sqlite3_free(zSql);
  return rc;
}

/* Release 1.0.0 declared the chunk and stat "sum" columns REAL, which forced
** every subtotal to floating point and lost precision on large integer sums.
** They are now typeless.  Detect the old declaration so it can be replaced. */
static int columnarHasLegacySumColumn(sqlite3 *db, const ColumnarTableRef *pRef, int *pbLegacy){
  sqlite3_stmt *pStmt = 0;
  char *zSql;
  int rc;

  *pbLegacy = 0;
  zSql = sqlite3_mprintf("PRAGMA \"%w\".table_info(\"%w__columnar_chunk\")", pRef->zDb, pRef->zTab);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, 0);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ) return rc;
  while( (rc = sqlite3_step(pStmt))==SQLITE_ROW ){
    const char *zName = (const char*)sqlite3_column_text(pStmt, 1);
    if( zName && sqlite3_stricmp(zName, "sum")==0 ){
      const char *zType = (const char*)sqlite3_column_text(pStmt, 2);
      *pbLegacy = zType!=0 && zType[0]!=0;
      break;
    }
  }
  if( rc==SQLITE_ROW || rc==SQLITE_DONE ) rc = SQLITE_OK;
  sqlite3_finalize(pStmt);
  return rc;
}

/* Replace the legacy chunk and stat tables with ones whose sum column can hold
** an exact integer.  Both hold nothing but data derived from the column
** tables, so dropping them is safe: clearing the chunk count sends the caller
** down its rebuild-everything path and the stats come back correct. */
static int columnarMigrateSumColumns(sqlite3 *db, const ColumnarTableRef *pRef, char **pzErr){
  char *zSql;
  int rc;

  /* The first drop doubles as a probe: if the schema cannot be changed here,
  ** leave the old tables in place and keep the previous behaviour rather than
  ** failing the call.  Their sums stay floating point, which is what this
  ** database has always returned. */
  zSql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%w\".\"%w__columnar_chunk\"", pRef->zDb, pRef->zTab);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = columnarExec(db, 0, zSql);
  sqlite3_free(zSql);
  if( rc!=SQLITE_OK ) return SQLITE_OK;

  /* Past this point the chunk table is gone, so errors must be reported. */
  zSql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%w\".\"%w__columnar_stat\"", pRef->zDb, pRef->zTab);
  if( zSql==0 ) return SQLITE_NOMEM;
  rc = columnarExec(db, pzErr, zSql);
  sqlite3_free(zSql);
  if( rc==SQLITE_OK ) rc = columnarEnsureChunkTable(db, pRef, pzErr);
  if( rc==SQLITE_OK ) rc = columnarEnsureStatTable(db, pRef, pzErr);
  if( rc==SQLITE_OK ){
    zSql = sqlite3_mprintf("DELETE FROM \"%w\".\"%w__columnar_dirty\"", pRef->zDb, pRef->zTab);
    if( zSql==0 ) return SQLITE_NOMEM;
    rc = columnarExec(db, pzErr, zSql);
    sqlite3_free(zSql);
  }
  if( rc==SQLITE_OK ) rc = columnarMetaSetInt(db, pRef, "chunk_count", 0);
  if( rc==SQLITE_OK ) rc = columnarMetaSetInt(db, pRef, "dirty_count", 0);
  if( rc==SQLITE_OK ) rc = columnarMetaSetInt(db, pRef, "stats_valid", 0);
  return rc;
}

static void columnarAnalyzeFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  ColumnarTableRef sRef = {0, 0};
  const char *zDb = 0;
  const char *zTab = 0;
  char *zErr = 0;
  sqlite3_stmt *pScan = 0;
  sqlite3_stmt *pIns = 0;
  char *zSql = 0;
  sqlite3_int64 nRow = 0;
  sqlite3_int64 nExpectedChunk = 0;
  sqlite3_int64 nDirtyTotal = 0;
  sqlite3_int64 nChunkTotal = 0;
  sqlite3_int64 nAnalyzed = 0;
  sqlite3_int64 nStatsValid = 0;
  int bCheckMissing = 0;
  int bFound = 0;
  int nCol = 0;
  int i;
  int rc;

  if( argc!=1 ){
    sqlite3_result_error(ctx, "wrong number of arguments", -1);
    return;
  }
  rc = columnarParseTableArg(ctx, argv[0], &sRef);
  if( rc!=SQLITE_OK ) return;
  zDb = sRef.zDb;
  zTab = sRef.zTab;
  if( zDb==0 || zTab==0 ){
    sqlite3_result_error(ctx, "invalid columnar_analyze arguments", -1);
    return;
  }

  rc = columnarRequireColumnarTable(db, &sRef, &zErr);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarEnsureMetaTable(db, &sRef, &zErr);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarMetaGetInt(db, &sRef, "ncol", &nStatsValid, &bFound);
  if( rc!=SQLITE_OK ) goto analyze_error;
  if( bFound && nStatsValid>0 ){
    nCol = (int)nStatsValid;
  }else{
    rc = columnarColumnCount(db, &sRef, &nCol, &zErr);
    if( rc!=SQLITE_OK ) goto analyze_error;
  }
  rc = columnarMetaEnsureBase(db, &sRef, nCol);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarEnsureChunkTable(db, &sRef, &zErr);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarEnsureDirtyTable(db, &sRef, &zErr);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarEnsureStatTable(db, &sRef, &zErr);
  if( rc!=SQLITE_OK ) goto analyze_error;
  {
    int bLegacy = 0;
    rc = columnarHasLegacySumColumn(db, &sRef, &bLegacy);
    if( rc!=SQLITE_OK ) goto analyze_error;
    if( bLegacy ){
      rc = columnarMigrateSumColumns(db, &sRef, &zErr);
      if( rc!=SQLITE_OK ) goto analyze_error;
    }
  }

  rc = columnarMetaGetInt(db, &sRef, "dirty_count", &nDirtyTotal, &bFound);
  if( rc!=SQLITE_OK ) goto analyze_error;
  if( !bFound ){
    rc = columnarInt64Query(db, &nDirtyTotal, "SELECT count(*) FROM \"%w\".\"%w__columnar_dirty\"", zDb, zTab);
    if( rc!=SQLITE_OK ) goto analyze_error;
    rc = columnarMetaSetInt(db, &sRef, "dirty_count", nDirtyTotal);
    if( rc!=SQLITE_OK ) goto analyze_error;
  }
  rc = columnarMetaGetInt(db, &sRef, "chunk_count", &nChunkTotal, &bFound);
  if( rc!=SQLITE_OK ) goto analyze_error;
  if( !bFound ){
    rc = columnarInt64Query(db, &nChunkTotal, "SELECT count(*) FROM \"%w\".\"%w__columnar_chunk\" WHERE col=0", zDb, zTab);
    if( rc!=SQLITE_OK ) goto analyze_error;
    rc = columnarMetaSetInt(db, &sRef, "chunk_count", nChunkTotal);
    if( rc!=SQLITE_OK ) goto analyze_error;
  }
  rc = columnarMetaGetInt(db, &sRef, "stats_valid", &nStatsValid, &bFound);
  if( rc!=SQLITE_OK ) goto analyze_error;
  if( bFound && nStatsValid && nDirtyTotal==0 ){
    sqlite3_result_int64(ctx, 0);
    columnarClearTableRef(&sRef);
    return;
  }
  bCheckMissing = nChunkTotal==0;
  if( bCheckMissing ){
    rc = columnarInt64Query(db, &nRow, "SELECT count(*) FROM \"%w\".\"%w__columnar_rowid\"", zDb, zTab);
    if( rc!=SQLITE_OK ) goto analyze_error;
    rc = columnarInt64Query(db, &nExpectedChunk, "SELECT count(*) FROM (" " SELECT DISTINCT ((rid-1)/%d) FROM \"%w\".\"%w__columnar_rowid\"" ")", COLUMNAR_CHUNK_SIZE, zDb, zTab);
    if( rc!=SQLITE_OK ) goto analyze_error;
    nAnalyzed = nExpectedChunk * nCol;
  }else{
    rc = columnarMetaGetInt(db, &sRef, "row_count", &nRow, &bFound);
    if( rc!=SQLITE_OK ) goto analyze_error;
    if( !bFound ){
      rc = columnarInt64Query(db, &nRow, "SELECT count(*) FROM \"%w\".\"%w__columnar_rowid\"", zDb, zTab);
      if( rc!=SQLITE_OK ) goto analyze_error;
    }
    nAnalyzed = nDirtyTotal;
  }

  rc = columnarPrepare(db, &pIns, &zErr, "REPLACE INTO \"%w\".\"%w__columnar_stat\"" "(col,row_count,nonnull_count,sum,min,max)" " VALUES(?1,?2,?3,?4,?5,?6)", zDb, zTab);
  if( rc!=SQLITE_OK ) goto analyze_error;

  zSql = sqlite3_mprintf("DELETE FROM \"%w\".\"%w__columnar_stat\"", zDb, zTab);
  if( zSql==0 ){
    rc = SQLITE_NOMEM;
    goto analyze_error;
  }
  rc = columnarExec(db, &zErr, zSql);
  sqlite3_free(zSql);
  zSql = 0;
  if( rc!=SQLITE_OK ) goto analyze_error;

  for(i=0; i<nCol; i++){
    sqlite3_int64 nDirtyChunk = 0;
    sqlite3_int64 nKnownChunk = 0;

    if( nDirtyTotal>0 ){
      rc = columnarInt64Query(db, &nDirtyChunk, "SELECT count(*) FROM \"%w\".\"%w__columnar_dirty\" WHERE col=%d", zDb, zTab, i);
      if( rc!=SQLITE_OK ) goto analyze_error;
    }
    if( nDirtyChunk>0 ){
      zSql = sqlite3_mprintf("DELETE FROM \"%w\".\"%w__columnar_chunk\"" " WHERE col=%d AND chunk IN (" "   SELECT chunk FROM \"%w\".\"%w__columnar_dirty\" WHERE col=%d" ")", zDb, zTab, i, zDb, zTab, i);
      if( zSql==0 ){
        rc = SQLITE_NOMEM;
        goto analyze_error;
      }
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
      zSql = 0;
      if( rc!=SQLITE_OK ) goto analyze_error;

      zSql = sqlite3_mprintf("INSERT OR REPLACE INTO \"%w\".\"%w__columnar_chunk\"" "(col,chunk,rid_min,rid_max,row_count,nonnull_count,sum,min,max)" " SELECT %d, d.chunk, min(x.rid), max(x.rid)," " count(*), count(x.v), sum(x.v), min(x.v), max(x.v)" " FROM \"%w\".\"%w__columnar_dirty\" AS d" " JOIN \"%w\".\"%w__columnar_c%d\" AS x" "   ON x.rid BETWEEN d.chunk*%d+1 AND (d.chunk+1)*%d" " WHERE d.col=%d" " GROUP BY d.chunk", zDb, zTab, i, zDb, zTab, zDb, zTab, i, COLUMNAR_CHUNK_SIZE, COLUMNAR_CHUNK_SIZE, i);
      if( zSql==0 ){
        rc = SQLITE_NOMEM;
        goto analyze_error;
      }
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
      zSql = 0;
      if( rc!=SQLITE_OK ) goto analyze_error;
    }

    if( bCheckMissing ){
      rc = columnarInt64Query(db, &nKnownChunk, "SELECT count(*) FROM \"%w\".\"%w__columnar_chunk\" WHERE col=%d", zDb, zTab, i);
      if( rc!=SQLITE_OK ) goto analyze_error;
    }
    if( bCheckMissing && nKnownChunk<nExpectedChunk ){
      zSql = sqlite3_mprintf("INSERT INTO \"%w\".\"%w__columnar_chunk\"" "(col,chunk,rid_min,rid_max,row_count,nonnull_count,sum,min,max)" " SELECT %d, ((rid-1)/%d), min(rid), max(rid)," " count(*), count(v), sum(v), min(v), max(v)" " FROM \"%w\".\"%w__columnar_c%d\" AS x" " WHERE NOT EXISTS (" "   SELECT 1 FROM \"%w\".\"%w__columnar_chunk\" AS c" "    WHERE c.col=%d AND c.chunk=((x.rid-1)/%d)" ")" " GROUP BY ((rid-1)/%d)", zDb, zTab, i, COLUMNAR_CHUNK_SIZE, zDb, zTab, i, zDb, zTab, i, COLUMNAR_CHUNK_SIZE, COLUMNAR_CHUNK_SIZE);
      if( zSql==0 ){
        rc = SQLITE_NOMEM;
        goto analyze_error;
      }
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
      zSql = 0;
      if( rc!=SQLITE_OK ) goto analyze_error;
    }

    if( nDirtyChunk>0 ){
      zSql = sqlite3_mprintf("DELETE FROM \"%w\".\"%w__columnar_dirty\" WHERE col=%d", zDb, zTab, i);
      if( zSql==0 ){
        rc = SQLITE_NOMEM;
        goto analyze_error;
      }
      rc = columnarExec(db, &zErr, zSql);
      sqlite3_free(zSql);
      zSql = 0;
      if( rc!=SQLITE_OK ) goto analyze_error;
    }

    rc = columnarPrepare(db, &pScan, &zErr, "SELECT coalesce(sum(nonnull_count),0),sum(sum),min(min),max(max)" " FROM \"%w\".\"%w__columnar_chunk\" WHERE col=%d", zDb, zTab, i);
    if( rc!=SQLITE_OK ) goto analyze_error;
    rc = sqlite3_step(pScan);
    if( rc!=SQLITE_ROW ){
      rc = rc==SQLITE_DONE ? SQLITE_CORRUPT : rc;
      goto analyze_error;
    }
    sqlite3_bind_int(pIns, 1, i);
    sqlite3_bind_int64(pIns, 2, nRow);
    sqlite3_bind_int64(pIns, 3, sqlite3_column_int64(pScan, 0));
    sqlite3_bind_value(pIns, 4, sqlite3_column_value(pScan, 1));
    sqlite3_bind_value(pIns, 5, sqlite3_column_value(pScan, 2));
    sqlite3_bind_value(pIns, 6, sqlite3_column_value(pScan, 3));
    rc = columnarStepDone(pIns);
    sqlite3_reset(pIns);
    sqlite3_clear_bindings(pIns);
    sqlite3_finalize(pScan);
    pScan = 0;
    if( rc!=SQLITE_OK ) goto analyze_error;
  }
  sqlite3_finalize(pIns);
  pIns = 0;
  if( bCheckMissing ){
    nChunkTotal = nExpectedChunk;
  }else{
    rc = columnarInt64Query(db, &nChunkTotal, "SELECT count(*) FROM \"%w\".\"%w__columnar_chunk\" WHERE col=0", zDb, zTab);
    if( rc!=SQLITE_OK ) goto analyze_error;
  }
  rc = columnarMetaSetInt(db, &sRef, "row_count", nRow);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarMetaSetInt(db, &sRef, "chunk_count", nChunkTotal);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarMetaSetInt(db, &sRef, "dirty_count", 0);
  if( rc!=SQLITE_OK ) goto analyze_error;
  rc = columnarMetaSetInt(db, &sRef, "stats_valid", 1);
  if( rc!=SQLITE_OK ) goto analyze_error;
  sqlite3_result_int64(ctx, nAnalyzed);
  columnarClearTableRef(&sRef);
  return;

analyze_error:
  sqlite3_finalize(pScan);
  sqlite3_finalize(pIns);
  sqlite3_free(zSql);
  sqlite3_result_error(ctx, zErr ? zErr : sqlite3_errmsg(db), -1);
  sqlite3_free(zErr);
  columnarClearTableRef(&sRef);
}

static int columnarRangeArgs(sqlite3_context *ctx, int argc, sqlite3_value **argv, ColumnarTableRef *pRef, int *piValCol, int *piFilterCol, sqlite3_value **ppLo, sqlite3_value **ppHi){
  int aCol[2];
  int rc;

  rc = columnarParseTableColumnsArgs(ctx, argc, argv, pRef, 2, 2, aCol);
  if( rc!=SQLITE_OK ) return rc;
  *piValCol = aCol[0];
  *piFilterCol = aCol[1];
  *ppLo = argv[3];
  *ppHi = argv[4];
  if( *piValCol<0 || *piFilterCol<0 ){
    sqlite3_result_error(ctx, "invalid columnar range arguments", -1);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

// PRAGMA MARK: - Range Analytics -

static int columnarChunkCountForColumn(sqlite3 *db, const ColumnarTableRef *pRef, int iFilterCol, sqlite3_int64 *pnChunk){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(db, &pStmt, 0, "SELECT count(*) FROM \"%w\".\"%w__columnar_chunk\" WHERE col=?1", pRef->zDb, pRef->zTab);
  *pnChunk = 0;
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_int(pStmt, 1, iFilterCol);
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    *pnChunk = sqlite3_column_int64(pStmt, 0);
    rc = SQLITE_OK;
  }else if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
  }
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarDirtyCountForColumn(sqlite3 *db, const ColumnarTableRef *pRef, int iCol, sqlite3_int64 *pnDirty){
  sqlite3_stmt *pStmt = 0;
  int rc = columnarPrepare(db, &pStmt, 0, "SELECT count(*) FROM \"%w\".\"%w__columnar_dirty\" WHERE col=?1", pRef->zDb, pRef->zTab);
  *pnDirty = 0;
  if( rc!=SQLITE_OK ){
    /* Older databases may not have the dirty table yet. */
    return SQLITE_OK;
  }
  sqlite3_bind_int(pStmt, 1, iCol);
  rc = sqlite3_step(pStmt);
  if( rc==SQLITE_ROW ){
    *pnDirty = sqlite3_column_int64(pStmt, 0);
    rc = SQLITE_OK;
  }else if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
  }
  sqlite3_finalize(pStmt);
  return rc;
}

static int columnarRangeOpenScan(sqlite3 *db, sqlite3_stmt **ppScan, const ColumnarTableRef *pRef, int iValCol, int iFilterCol, int bChunked){
  if( bChunked ){
    return columnarPrepare(db, ppScan, 0, "SELECT v.v FROM \"%w\".\"%w__columnar_c%d\" AS f" " JOIN \"%w\".\"%w__columnar_c%d\" AS v ON v.rid=f.rid" " WHERE f.rid BETWEEN ?1 AND ?2 AND f.v BETWEEN ?3 AND ?4" " ORDER BY f.rid", pRef->zDb, pRef->zTab, iFilterCol, pRef->zDb, pRef->zTab, iValCol);
  }else{
    return columnarPrepare(db, ppScan, 0, "SELECT v.v FROM \"%w\".\"%w__columnar_c%d\" AS f" " JOIN \"%w\".\"%w__columnar_c%d\" AS v ON v.rid=f.rid" " WHERE f.v BETWEEN ?1 AND ?2" " ORDER BY f.rid", pRef->zDb, pRef->zTab, iFilterCol, pRef->zDb, pRef->zTab, iValCol);
  }
}

static int columnarRangeRunScan(sqlite3_stmt *pScan, int bChunked, sqlite3_int64 iRidMin, sqlite3_int64 iRidMax, sqlite3_value *pLo, sqlite3_value *pHi, int eOp, ColumnarSum *pAcc){
  int rc;
  sqlite3_reset(pScan);
  sqlite3_clear_bindings(pScan);
  if( bChunked ){
    sqlite3_bind_int64(pScan, 1, iRidMin);
    sqlite3_bind_int64(pScan, 2, iRidMax);
    sqlite3_bind_value(pScan, 3, pLo);
    sqlite3_bind_value(pScan, 4, pHi);
  }else{
    sqlite3_bind_value(pScan, 1, pLo);
    sqlite3_bind_value(pScan, 2, pHi);
  }
  while( (rc = sqlite3_step(pScan))==SQLITE_ROW ){
    if( sqlite3_column_type(pScan, 0)!=SQLITE_NULL ){
      if( eOp!=COLUMNAR_ANALYTIC_COUNT ){
        columnarSumAddColumn(pAcc, pScan, 0);
      }else{
        pAcc->nVal++;
      }
    }
  }
  return rc==SQLITE_DONE ? SQLITE_OK : rc;
}

/* Fold a whole chunk's precomputed stats in, for a chunk that the zone map
** says passes the filter in its entirety.  iCount and iSum are the result
** columns of pChunk holding the value column's nonnull_count and sum. */
static void columnarRangeUseChunkStats(int eOp, sqlite3_stmt *pChunk, int iCount, int iSum, ColumnarSum *pAcc){
  sqlite3_int64 nChunk = sqlite3_column_int64(pChunk, iCount);
  if( nChunk<=0 ) return;
  if( eOp==COLUMNAR_ANALYTIC_COUNT ){
    pAcc->nVal += nChunk;
    return;
  }
  if( sqlite3_column_type(pChunk, iSum)==SQLITE_NULL ) return;
  if( sqlite3_column_type(pChunk, iSum)==SQLITE_INTEGER ){
    columnarSumAddInt(pAcc, sqlite3_column_int64(pChunk, iSum), nChunk);
  }else{
    columnarSumAddDouble(pAcc, sqlite3_column_double(pChunk, iSum), nChunk);
  }
}

static int columnarRangeCompute(sqlite3 *db, const ColumnarTableRef *pRef, int iValCol, int iFilterCol, sqlite3_value *pLo, sqlite3_value *pHi, int eOp, ColumnarSum *pAcc){
  sqlite3_stmt *pChunk = 0;
  sqlite3_stmt *pScan = 0;
  sqlite3_int64 nChunk = 0;
  sqlite3_int64 nDirtyFilter = 0;
  sqlite3_int64 nDirtyValue = 0;
  int bZoneOk = 0;
  int rc;

  columnarSumInit(pAcc);
  rc = columnarDirtyCountForColumn(db, pRef, iFilterCol, &nDirtyFilter);
  if( rc!=SQLITE_OK ) return rc;
  rc = columnarDirtyCountForColumn(db, pRef, iValCol, &nDirtyValue);
  if( rc!=SQLITE_OK ) return rc;
  if( nDirtyFilter>0 || nDirtyValue>0 ){
    rc = columnarRangeOpenScan(db, &pScan, pRef, iValCol, iFilterCol, 0);
    if( rc==SQLITE_OK ){
      rc = columnarRangeRunScan(pScan, 0, 0, 0, pLo, pHi, eOp, pAcc);
    }
    sqlite3_finalize(pScan);
    return rc;
  }
  rc = columnarChunkCountForColumn(db, pRef, iFilterCol, &nChunk);
  if( rc==SQLITE_OK && nChunk>0 ){
    rc = columnarZoneMapUsable(db, pRef, iFilterCol, pLo, pHi, &bZoneOk);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( rc!=SQLITE_OK || nChunk==0 || !bZoneOk ){
    rc = columnarRangeOpenScan(db, &pScan, pRef, iValCol, iFilterCol, 0);
    if( rc==SQLITE_OK ){
      rc = columnarRangeRunScan(pScan, 0, 0, 0, pLo, pHi, eOp, pAcc);
    }
    sqlite3_finalize(pScan);
    return rc;
  }

  rc = columnarPrepare(db, &pChunk, 0, "SELECT f.rid_min,f.rid_max," " (f.nonnull_count=f.row_count AND f.min>=?2 AND f.max<=?3)," " v.nonnull_count,v.sum" " FROM \"%w\".\"%w__columnar_chunk\" AS f" " JOIN \"%w\".\"%w__columnar_chunk\" AS v" "   ON v.col=?4 AND v.chunk=f.chunk" " WHERE f.col=?1 AND f.min IS NOT NULL AND f.max>=?2 AND f.min<=?3" " ORDER BY f.chunk", pRef->zDb, pRef->zTab, pRef->zDb, pRef->zTab);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_int(pChunk, 1, iFilterCol);
  sqlite3_bind_value(pChunk, 2, pLo);
  sqlite3_bind_value(pChunk, 3, pHi);
  sqlite3_bind_int(pChunk, 4, iValCol);

  rc = columnarRangeOpenScan(db, &pScan, pRef, iValCol, iFilterCol, 1);
  if( rc!=SQLITE_OK ) goto range_compute_done;

  while( (rc = sqlite3_step(pChunk))==SQLITE_ROW ){
    if( sqlite3_column_int(pChunk, 2) ){
      columnarRangeUseChunkStats(eOp, pChunk, 3, 4, pAcc);
    }else{
      rc = columnarRangeRunScan(pScan, 1, sqlite3_column_int64(pChunk, 0), sqlite3_column_int64(pChunk, 1), pLo, pHi, eOp, pAcc);
      if( rc!=SQLITE_OK ) goto range_compute_done;
    }
  }
  if( rc==SQLITE_DONE ) rc = SQLITE_OK;

range_compute_done:
  sqlite3_finalize(pChunk);
  sqlite3_finalize(pScan);
  return rc;
}

static void columnarRangeAnalyticFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  sqlite3 *db = sqlite3_context_db_handle(ctx);
  const ColumnarRangeAnalytic *pAnalytic =
      (const ColumnarRangeAnalytic*)sqlite3_user_data(ctx);
  ColumnarTableRef sRef = {0, 0};
  sqlite3_value *pLo = 0;
  sqlite3_value *pHi = 0;
  int iValCol = -1;
  int iFilterCol = -1;
  ColumnarSum sAcc;
  int rc;

  columnarSumInit(&sAcc);
  rc = columnarRangeArgs(ctx, argc, argv, &sRef, &iValCol, &iFilterCol, &pLo, &pHi);
  if( rc!=SQLITE_OK ) return;
  rc = columnarRangeCompute(db, &sRef, iValCol, iFilterCol, pLo, pHi, pAnalytic->eOp, &sAcc);
  if( rc!=SQLITE_OK ){
    sqlite3_result_error(ctx, sqlite3_errmsg(db), -1);
    columnarClearTableRef(&sRef);
    return;
  }
  columnarClearTableRef(&sRef);
  switch( pAnalytic->eOp ){
    case COLUMNAR_ANALYTIC_SUM:
      columnarSumResult(ctx, &sAcc);
      break;
    case COLUMNAR_ANALYTIC_AVG:
      columnarAvgResult(ctx, &sAcc);
      break;
    default:
      assert( pAnalytic->eOp==COLUMNAR_ANALYTIC_COUNT );
      sqlite3_result_int64(ctx, sAcc.nVal);
      break;
  }
}

static unsigned int columnarHash64(sqlite3_int64 x){
  sqlite3_uint64 u = (sqlite3_uint64)x;
  u ^= u >> 33;
  u *= (sqlite3_uint64)0xff51afd7ed558ccdULL;
  u ^= u >> 33;
  u *= (sqlite3_uint64)0xc4ceb9fe1a85ec53ULL;
  u ^= u >> 33;
  return (unsigned int)u;
}

// PRAGMA MARK: - Grouping -

static unsigned int columnarHashBytes(const unsigned char *aData, int nData, unsigned int h){
  int i;
  for(i=0; i<nData; i++){
    h ^= aData[i];
    h *= 16777619u;
  }
  return h;
}

static int columnarDoubleToInt64(double r, sqlite3_int64 *pi){
  sqlite3_int64 i;
  if( r<-9223372036854775807.0 || r>=9223372036854775807.0 ) return 0;
  i = (sqlite3_int64)r;
  if( (double)i!=r ) return 0;
  *pi = i;
  return 1;
}

static unsigned int columnarGroupKeyHash(sqlite3_value *pKey){
  int eType = sqlite3_value_type(pKey);
  switch( eType ){
    case SQLITE_NULL:
      return 0x811c9dc5u ^ 0x9e3779b9u;
    case SQLITE_INTEGER:
      return columnarHash64(sqlite3_value_int64(pKey));
    case SQLITE_FLOAT: {
      sqlite3_int64 iKey;
      double rKey = sqlite3_value_double(pKey);
      if( columnarDoubleToInt64(rKey, &iKey) ){
        return columnarHash64(iKey);
      }else{
        return columnarHashBytes((const unsigned char*)&rKey, (int)sizeof(rKey), 0x811c9dc5u ^ 0x46a3u);
      }
    }
    case SQLITE_TEXT: {
      const unsigned char *z = sqlite3_value_text(pKey);
      int n = z ? sqlite3_value_bytes(pKey) : 0;
      return columnarHashBytes(z, n, 0x811c9dc5u ^ 0x54u);
    }
    default: {
      const unsigned char *a = sqlite3_value_blob(pKey);
      int n = a ? sqlite3_value_bytes(pKey) : 0;
      return columnarHashBytes(a, n, 0x811c9dc5u ^ 0x42u);
    }
  }
}

static void columnarGroupEntryClear(ColumnarGroupEntry *pEntry){
  columnarValueClear(&pEntry->sKey);
  columnarValueClear(&pEntry->sMin);
  columnarValueClear(&pEntry->sMax);
}

static void columnarGroupEntriesClear(ColumnarGroupCursor *pCur){
  int i;
  if( pCur->aEntry ){
    for(i=0; i<pCur->nAlloc; i++){
      if( pCur->aEntry[i].bUsed ){
        columnarGroupEntryClear(&pCur->aEntry[i]);
      }
    }
    sqlite3_free(pCur->aEntry);
  }
  pCur->aEntry = 0;
  pCur->nAlloc = 0;
  pCur->nEntry = 0;
  pCur->iEntry = 0;
  pCur->iRowid = 0;
}

static int columnarGroupGrow(ColumnarGroupCursor *pCur){
  int nNew = pCur->nAlloc ? pCur->nAlloc*2 : 1024;
  ColumnarGroupEntry *aNew;
  int i;

  aNew = sqlite3_malloc64(sizeof(ColumnarGroupEntry) * (sqlite3_uint64)nNew);
  if( aNew==0 ) return SQLITE_NOMEM;
  memset(aNew, 0, sizeof(ColumnarGroupEntry) * (size_t)nNew);

  for(i=0; i<pCur->nAlloc; i++){
    if( pCur->aEntry[i].bUsed ){
      unsigned int h = pCur->aEntry[i].uKeyHash & (unsigned int)(nNew-1);
      while( aNew[h].bUsed ) h = (h+1) & (unsigned int)(nNew-1);
      aNew[h] = pCur->aEntry[i];
    }
  }
  sqlite3_free(pCur->aEntry);
  pCur->aEntry = aNew;
  pCur->nAlloc = nNew;
  return SQLITE_OK;
}

/* Fold one row into the hash table.  pVal1/pVal2 are the raw aggregate inputs,
** or NULL when this aggregate takes no such argument; bCountStar asks
** COLUMNAR_GROUP_COUNT to count rows rather than non-NULL values.
*/
static int columnarGroupAdd(ColumnarGroupCursor *pCur, const ColumnarGroupAgg *pAgg, sqlite3_value *pKey, sqlite3_value *pVal1, sqlite3_value *pVal2, int bCountStar){
  ColumnarGroupEntry *pEntry;
  unsigned int h;
  unsigned int uKeyHash = columnarGroupKeyHash(pKey);
  int bVal1 = pVal1!=0 && sqlite3_value_type(pVal1)!=SQLITE_NULL;
  int bVal2 = pVal2!=0 && sqlite3_value_type(pVal2)!=SQLITE_NULL;
  int rc;

  if( pCur->nEntry*2 >= pCur->nAlloc ){
    rc = columnarGroupGrow(pCur);
    if( rc!=SQLITE_OK ) return rc;
  }
  h = uKeyHash & (unsigned int)(pCur->nAlloc-1);
  while( pCur->aEntry[h].bUsed ){
    if( pCur->aEntry[h].uKeyHash==uKeyHash
     && columnarValueEqual(&pCur->aEntry[h].sKey, pKey)
    ){
      pEntry = &pCur->aEntry[h];
      goto group_add_value;
    }
    h = (h+1) & (unsigned int)(pCur->nAlloc-1);
  }
  pEntry = &pCur->aEntry[h];
  pEntry->bUsed = 1;
  pEntry->uKeyHash = uKeyHash;
  columnarSumInit(&pEntry->sSum);
  rc = columnarValueSave(&pEntry->sKey, pKey);
  if( rc!=SQLITE_OK ){
    memset(pEntry, 0, sizeof(*pEntry));
    return rc;
  }
  pCur->nEntry++;

group_add_value:
  switch( pAgg->eOp ){
    case COLUMNAR_GROUP_COUNT:
      if( bCountStar || bVal1 ) pEntry->nCount++;
      break;
    case COLUMNAR_GROUP_SUM:
    case COLUMNAR_GROUP_AVG:
    case COLUMNAR_GROUP_SUM_AVG_COUNT:
      if( bVal1 ){
        columnarSumAddValue(&pEntry->sSum, pVal1);
        pEntry->nCount++;
      }
      break;
    case COLUMNAR_GROUP_MIN:
      if( bVal1 ){
        if( pEntry->sMin.eType==0
         || columnarValueCompare(&pEntry->sMin, pVal1)>0
        ){
          columnarValueClear(&pEntry->sMin);
          rc = columnarValueSave(&pEntry->sMin, pVal1);
          if( rc!=SQLITE_OK ) return rc;
        }
        pEntry->nCount++;
      }
      break;
    case COLUMNAR_GROUP_MIN_MAX_COUNT:
      if( bVal1 ){
        if( pEntry->sMin.eType==0
         || columnarValueCompare(&pEntry->sMin, pVal1)>0
        ){
          columnarValueClear(&pEntry->sMin);
          rc = columnarValueSave(&pEntry->sMin, pVal1);
          if( rc!=SQLITE_OK ) return rc;
        }
        if( pEntry->sMax.eType==0
         || columnarValueCompare(&pEntry->sMax, pVal1)<0
        ){
          columnarValueClear(&pEntry->sMax);
          rc = columnarValueSave(&pEntry->sMax, pVal1);
          if( rc!=SQLITE_OK ) return rc;
        }
        pEntry->nCount++;
      }
      break;
    case COLUMNAR_GROUP_MAX:
      if( bVal1 ){
        if( pEntry->sMax.eType==0
         || columnarValueCompare(&pEntry->sMax, pVal1)<0
        ){
          columnarValueClear(&pEntry->sMax);
          rc = columnarValueSave(&pEntry->sMax, pVal1);
          if( rc!=SQLITE_OK ) return rc;
        }
        pEntry->nCount++;
      }
      break;
    case COLUMNAR_GROUP_RANGE:
      if( bVal1 ){
        if( pEntry->sMax.eType==0
         || columnarValueCompare(&pEntry->sMax, pVal1)<0
        ){
          columnarValueClear(&pEntry->sMax);
          rc = columnarValueSave(&pEntry->sMax, pVal1);
          if( rc!=SQLITE_OK ) return rc;
        }
      }
      if( bVal2 ){
        if( pEntry->sMin.eType==0
         || columnarValueCompare(&pEntry->sMin, pVal2)>0
        ){
          columnarValueClear(&pEntry->sMin);
          rc = columnarValueSave(&pEntry->sMin, pVal2);
          if( rc!=SQLITE_OK ) return rc;
        }
      }
      if( bVal1 || bVal2 ) pEntry->nCount++;
      break;
  }
  return SQLITE_OK;
}

static void columnarGroupAdvanceToUsed(ColumnarGroupCursor *pCur){
  while( pCur->iEntry<pCur->nAlloc && !pCur->aEntry[pCur->iEntry].bUsed ){
    pCur->iEntry++;
  }
}

static int columnarGroupConnect(sqlite3 *db, void *pAux, int argc, const char *const*argv, sqlite3_vtab **ppVtab, char **pzErr){
  ColumnarGroupVtab *pNew;
  const ColumnarGroupAgg *pAgg = (const ColumnarGroupAgg*)pAux;
  int rc;
  (void)argc;
  (void)argv;
  (void)pzErr;

  if( pAgg==0 ) pAgg = &columnarGroupSumAgg;
  rc = sqlite3_declare_vtab(db, pAgg->zDecl);
  if( rc!=SQLITE_OK ) return rc;
  pNew = sqlite3_malloc64(sizeof(*pNew));
  if( pNew==0 ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(*pNew));
  pNew->db = db;
  pNew->pAgg = pAgg;
  *ppVtab = (sqlite3_vtab*)pNew;
  return SQLITE_OK;
}

// PRAGMA MARK: - Grouped Table-Valued Functions -

static int columnarGroupDisconnect(sqlite3_vtab *pVtab){
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

static int columnarGroupOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCur){
  ColumnarGroupCursor *pCur;
  (void)pVtab;
  pCur = sqlite3_malloc64(sizeof(*pCur));
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->iEntry = 0;
  *ppCur = (sqlite3_vtab_cursor*)pCur;
  return SQLITE_OK;
}

static int columnarGroupClose(sqlite3_vtab_cursor *cur){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  columnarGroupEntriesClear(pCur);
  sqlite3_free(pCur);
  return SQLITE_OK;
}

static int columnarGroupBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  ColumnarGroupVtab *pTab = (ColumnarGroupVtab*)tab;
  const ColumnarGroupAgg *pAgg = pTab->pAgg;
  int i;
  int iTab = pAgg->nOut;
  int iKey = iTab + 1;
  int iVal1 = iTab + 2;
  int iVal2 = pAgg->nValHidden>=2 ? iTab + 3 : -1;
  int cTab = -1;
  int cKey = -1;
  int cVal1 = -1;
  int cVal2 = -1;
  int nArg = 1;
  for(i=0; i<pIdxInfo->nConstraint; i++){
    const struct sqlite3_index_constraint *pCons = &pIdxInfo->aConstraint[i];
    if( !pCons->usable || pCons->op!=SQLITE_INDEX_CONSTRAINT_EQ ) continue;
    if( pCons->iColumn==iTab ) cTab = i;
    else if( pCons->iColumn==iKey ) cKey = i;
    else if( pCons->iColumn==iVal1 ) cVal1 = i;
    else if( iVal2>=0 && pCons->iColumn==iVal2 ) cVal2 = i;
  }
  if( cTab>=0 && cKey>=0
   && (pAgg->nValArg<1 || cVal1>=0)
   && (pAgg->nValArg<2 || cVal2>=0)
  ){
    int idxNum = 0;
    pIdxInfo->aConstraintUsage[cTab].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cTab].omit = 1;
    pIdxInfo->aConstraintUsage[cKey].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cKey].omit = 1;
    if( cVal1>=0 ){
      pIdxInfo->aConstraintUsage[cVal1].argvIndex = nArg++;
      pIdxInfo->aConstraintUsage[cVal1].omit = 1;
      idxNum |= COLUMNAR_GROUP_IDX_VAL1;
    }
    if( cVal2>=0 ){
      pIdxInfo->aConstraintUsage[cVal2].argvIndex = nArg++;
      pIdxInfo->aConstraintUsage[cVal2].omit = 1;
      idxNum |= COLUMNAR_GROUP_IDX_VAL2;
    }
    pIdxInfo->estimatedCost = 100000.0;
    pIdxInfo->estimatedRows = 1000;
    pIdxInfo->idxNum = idxNum;
  }else{
    pIdxInfo->estimatedCost = 1.0e99;
    pIdxInfo->estimatedRows = 2147483647;
  }
  return SQLITE_OK;
}

static int columnarGroupBuild(ColumnarGroupCursor *pCur, sqlite3 *db, const ColumnarTableRef *pRef, int iKeyCol, int iValCol1, int iValCol2, const ColumnarGroupAgg *pAgg){
  sqlite3_stmt *pKey = 0;
  sqlite3_stmt *pVal1 = 0;
  sqlite3_stmt *pVal2 = 0;
  char *zErr = 0;
  int rc;

  rc = columnarPrepare(db, &pKey, &zErr, "SELECT rid,v FROM \"%w\".\"%w__columnar_c%d\" ORDER BY rid", pRef->zDb, pRef->zTab, iKeyCol);
  if( rc!=SQLITE_OK ) goto group_build_done;
  if( iValCol1>=0 ){
    rc = columnarPrepare(db, &pVal1, &zErr, "SELECT rid,v FROM \"%w\".\"%w__columnar_c%d\" ORDER BY rid", pRef->zDb, pRef->zTab, iValCol1);
    if( rc!=SQLITE_OK ) goto group_build_done;
  }
  if( iValCol2>=0 ){
    rc = columnarPrepare(db, &pVal2, &zErr, "SELECT rid,v FROM \"%w\".\"%w__columnar_c%d\" ORDER BY rid", pRef->zDb, pRef->zTab, iValCol2);
    if( rc!=SQLITE_OK ) goto group_build_done;
  }

  while( 1 ){
    int rcKey = sqlite3_step(pKey);
    int rcVal1 = pVal1 ? sqlite3_step(pVal1) : SQLITE_DONE;
    int rcVal2 = pVal2 ? sqlite3_step(pVal2) : SQLITE_DONE;
    if( rcKey==SQLITE_DONE
     && (pVal1==0 || rcVal1==SQLITE_DONE)
     && (pVal2==0 || rcVal2==SQLITE_DONE)
    ){
      rc = SQLITE_OK;
      break;
    }
    if( rcKey!=SQLITE_ROW
     || (pVal1!=0 && rcVal1!=SQLITE_ROW)
     || (pVal2!=0 && rcVal2!=SQLITE_ROW)
    ){
      rc = rcKey;
      if( rc==SQLITE_ROW || rc==SQLITE_DONE ){
        rc = pVal1!=0 ? rcVal1 : rcVal2;
      }
      if( rc==SQLITE_ROW || rc==SQLITE_DONE ){
        rc = pVal2!=0 ? rcVal2 : SQLITE_CORRUPT;
      }
      if( rc==SQLITE_ROW || rc==SQLITE_DONE ) rc = SQLITE_CORRUPT;
      break;
    }
    if( pVal1!=0
     && sqlite3_column_int64(pKey, 0)!=sqlite3_column_int64(pVal1, 0)
    ){
      rc = SQLITE_CORRUPT;
      break;
    }
    if( pVal2!=0
     && sqlite3_column_int64(pKey, 0)!=sqlite3_column_int64(pVal2, 0)
    ){
      rc = SQLITE_CORRUPT;
      break;
    }
    rc = columnarGroupAdd(pCur, pAgg, sqlite3_column_value(pKey, 1), pVal1 ? sqlite3_column_value(pVal1, 1) : 0, pVal2 ? sqlite3_column_value(pVal2, 1) : 0, pVal1==0);
    if( rc!=SQLITE_OK ) break;
  }

group_build_done:
  sqlite3_finalize(pKey);
  sqlite3_finalize(pVal1);
  sqlite3_finalize(pVal2);
  sqlite3_free(zErr);
  return rc;
}

static int columnarGroupFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  ColumnarGroupVtab *pTab = (ColumnarGroupVtab*)cur->pVtab;
  const ColumnarGroupAgg *pAgg = pTab->pAgg;
  ColumnarTableRef sRef = {0, 0};
  int iKeyCol = -1;
  int iValCol1 = -1;
  int iValCol2 = -1;
  int aCol[3];
  int nColArg = 1;
  char *zErr = 0;
  int rc;
  sqlite3_vtab *pVtab = cur->pVtab;
  (void)idxStr;

  columnarGroupEntriesClear(pCur);
  cur->pVtab = pVtab;

  if( argc<2+pAgg->nValArg ){
    sqlite3_free(cur->pVtab->zErrMsg);
    cur->pVtab->zErrMsg = sqlite3_mprintf("%s requires table, key column%s%s", pAgg->zErrName, pAgg->nValArg>=1 ? ", and value column" : "", pAgg->nValArg>=2 ? "s" : "");
    return SQLITE_ERROR;
  }
  if( idxNum&COLUMNAR_GROUP_IDX_VAL1 ) nColArg++;
  if( idxNum&COLUMNAR_GROUP_IDX_VAL2 ) nColArg++;
  rc = columnarParseVtabTableColumns(pTab->db, argv, &sRef, nColArg, aCol, &zErr);
  if( rc!=SQLITE_OK ) goto group_filter_error;
  iKeyCol = aCol[0];
  if( idxNum&COLUMNAR_GROUP_IDX_VAL1 ){
    iValCol1 = aCol[1];
  }
  if( idxNum&COLUMNAR_GROUP_IDX_VAL2 ){
    iValCol2 = aCol[nColArg-1];
  }

  rc = columnarGroupBuild(pCur, pTab->db, &sRef, iKeyCol, iValCol1, iValCol2, pAgg);
  if( rc!=SQLITE_OK ) goto group_filter_error;
  columnarGroupAdvanceToUsed(pCur);
  columnarClearTableRef(&sRef);
  return SQLITE_OK;

group_filter_error:
  sqlite3_free(cur->pVtab->zErrMsg);
  cur->pVtab->zErrMsg = sqlite3_mprintf("%s", zErr ? zErr : sqlite3_errmsg(pTab->db));
  sqlite3_free(zErr);
  columnarClearTableRef(&sRef);
  return rc;
}

static int columnarGroupWhereBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo){
  ColumnarGroupVtab *pTab = (ColumnarGroupVtab*)tab;
  const ColumnarGroupAgg *pAgg = pTab->pAgg;
  int i;
  int iTab = pAgg->nOut;
  int iKey = iTab + 1;
  int iVal = iTab + 2;
  int iFilter = iTab + 3;
  int iLo = iTab + 4;
  int iHi = iTab + 5;
  int cTab = -1;
  int cKey = -1;
  int cVal = -1;
  int cFilter = -1;
  int cLo = -1;
  int cHi = -1;
  int nArg = 1;

  for(i=0; i<pIdxInfo->nConstraint; i++){
    const struct sqlite3_index_constraint *pCons = &pIdxInfo->aConstraint[i];
    if( !pCons->usable || pCons->op!=SQLITE_INDEX_CONSTRAINT_EQ ) continue;
    if( pCons->iColumn==iTab ) cTab = i;
    else if( pCons->iColumn==iKey ) cKey = i;
    else if( pCons->iColumn==iVal ) cVal = i;
    else if( pCons->iColumn==iFilter ) cFilter = i;
    else if( pCons->iColumn==iLo ) cLo = i;
    else if( pCons->iColumn==iHi ) cHi = i;
  }
  if( cTab>=0 && cKey>=0 && cVal>=0 && cFilter>=0 && cLo>=0 && cHi>=0 ){
    int idxNum = 0;
    pIdxInfo->aConstraintUsage[cTab].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cTab].omit = 1;
    pIdxInfo->aConstraintUsage[cKey].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cKey].omit = 1;
    pIdxInfo->aConstraintUsage[cVal].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cVal].omit = 1;
    pIdxInfo->aConstraintUsage[cFilter].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cFilter].omit = 1;
    pIdxInfo->aConstraintUsage[cLo].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cLo].omit = 1;
    pIdxInfo->aConstraintUsage[cHi].argvIndex = nArg++;
    pIdxInfo->aConstraintUsage[cHi].omit = 1;
    pIdxInfo->idxNum = idxNum;
    pIdxInfo->estimatedCost = 10000.0;
    pIdxInfo->estimatedRows = 1000;
  }else{
    pIdxInfo->estimatedCost = 1.0e99;
    pIdxInfo->estimatedRows = 2147483647;
  }
  return SQLITE_OK;
}

static int columnarGroupWhereOpenScan(sqlite3 *db, sqlite3_stmt **ppScan, const ColumnarTableRef *pRef, int iKeyCol, int iValCol, int iFilterCol, int bChunked){
  if( bChunked ){
    return columnarPrepare(db, ppScan, 0, "SELECT k.v, v.v FROM \"%w\".\"%w__columnar_c%d\" AS f" " JOIN \"%w\".\"%w__columnar_c%d\" AS k ON k.rid=f.rid" " JOIN \"%w\".\"%w__columnar_c%d\" AS v ON v.rid=f.rid" " WHERE f.rid BETWEEN ?1 AND ?2 AND f.v BETWEEN ?3 AND ?4" " ORDER BY f.rid", pRef->zDb, pRef->zTab, iFilterCol, pRef->zDb, pRef->zTab, iKeyCol, pRef->zDb, pRef->zTab, iValCol);
  }else{
    return columnarPrepare(db, ppScan, 0, "SELECT k.v, v.v FROM \"%w\".\"%w__columnar_c%d\" AS f" " JOIN \"%w\".\"%w__columnar_c%d\" AS k ON k.rid=f.rid" " JOIN \"%w\".\"%w__columnar_c%d\" AS v ON v.rid=f.rid" " WHERE f.v BETWEEN ?1 AND ?2" " ORDER BY f.rid", pRef->zDb, pRef->zTab, iFilterCol, pRef->zDb, pRef->zTab, iKeyCol, pRef->zDb, pRef->zTab, iValCol);
  }
}

static int columnarGroupWhereOpenFullScan(sqlite3 *db, sqlite3_stmt **ppScan, const ColumnarTableRef *pRef, int iKeyCol, int iValCol){
  return columnarPrepare(db, ppScan, 0, "SELECT k.v, v.v FROM \"%w\".\"%w__columnar_c%d\" AS k" " JOIN \"%w\".\"%w__columnar_c%d\" AS v ON v.rid=k.rid" " WHERE k.rid BETWEEN ?1 AND ?2" " ORDER BY k.rid", pRef->zDb, pRef->zTab, iKeyCol, pRef->zDb, pRef->zTab, iValCol);
}

static int columnarGroupWhereRunScan(sqlite3_stmt *pScan, int bChunked, sqlite3_int64 iRidMin, sqlite3_int64 iRidMax, sqlite3_value *pLo, sqlite3_value *pHi, ColumnarGroupCursor *pCur, const ColumnarGroupAgg *pAgg){
  int rc;
  sqlite3_reset(pScan);
  sqlite3_clear_bindings(pScan);
  if( bChunked ){
    sqlite3_bind_int64(pScan, 1, iRidMin);
    sqlite3_bind_int64(pScan, 2, iRidMax);
    sqlite3_bind_value(pScan, 3, pLo);
    sqlite3_bind_value(pScan, 4, pHi);
  }else{
    sqlite3_bind_value(pScan, 1, pLo);
    sqlite3_bind_value(pScan, 2, pHi);
  }
  while( (rc = sqlite3_step(pScan))==SQLITE_ROW ){
    rc = columnarGroupAdd(pCur, pAgg, sqlite3_column_value(pScan, 0), sqlite3_column_value(pScan, 1), 0, 0);
    if( rc!=SQLITE_OK ) return rc;
  }
  return rc==SQLITE_DONE ? SQLITE_OK : rc;
}

static int columnarGroupWhereRunFullScan(sqlite3_stmt *pScan, sqlite3_int64 iRidMin, sqlite3_int64 iRidMax, ColumnarGroupCursor *pCur, const ColumnarGroupAgg *pAgg){
  int rc;
  sqlite3_reset(pScan);
  sqlite3_clear_bindings(pScan);
  sqlite3_bind_int64(pScan, 1, iRidMin);
  sqlite3_bind_int64(pScan, 2, iRidMax);
  while( (rc = sqlite3_step(pScan))==SQLITE_ROW ){
    rc = columnarGroupAdd(pCur, pAgg, sqlite3_column_value(pScan, 0), sqlite3_column_value(pScan, 1), 0, 0);
    if( rc!=SQLITE_OK ) return rc;
  }
  return rc==SQLITE_DONE ? SQLITE_OK : rc;
}

static int columnarGroupBuildWhereRange(ColumnarGroupCursor *pCur, sqlite3 *db, const ColumnarTableRef *pRef, int iKeyCol, int iValCol, int iFilterCol, sqlite3_value *pLo, sqlite3_value *pHi, const ColumnarGroupAgg *pAgg){
  sqlite3_stmt *pChunk = 0;
  sqlite3_stmt *pScan = 0;
  sqlite3_stmt *pFullScan = 0;
  sqlite3_int64 nChunk = 0;
  sqlite3_int64 nDirty = 0;
  int bZoneOk = 0;
  int rc;

  rc = columnarDirtyCountForColumn(db, pRef, iKeyCol, &nDirty);
  if( rc!=SQLITE_OK ) return rc;
  if( nDirty==0 ){
    rc = columnarDirtyCountForColumn(db, pRef, iValCol, &nDirty);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( nDirty==0 ){
    rc = columnarDirtyCountForColumn(db, pRef, iFilterCol, &nDirty);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( nDirty>0 ){
    rc = columnarGroupWhereOpenScan(db, &pScan, pRef, iKeyCol, iValCol, iFilterCol, 0);
    if( rc==SQLITE_OK ){
      rc = columnarGroupWhereRunScan(pScan, 0, 0, 0, pLo, pHi, pCur, pAgg);
    }
    sqlite3_finalize(pScan);
    return rc;
  }

  rc = columnarChunkCountForColumn(db, pRef, iFilterCol, &nChunk);
  if( rc==SQLITE_OK && nChunk>0 ){
    rc = columnarZoneMapUsable(db, pRef, iFilterCol, pLo, pHi, &bZoneOk);
    if( rc!=SQLITE_OK ) return rc;
  }
  if( rc!=SQLITE_OK || nChunk==0 || !bZoneOk ){
    rc = columnarGroupWhereOpenScan(db, &pScan, pRef, iKeyCol, iValCol, iFilterCol, 0);
    if( rc==SQLITE_OK ){
      rc = columnarGroupWhereRunScan(pScan, 0, 0, 0, pLo, pHi, pCur, pAgg);
    }
    sqlite3_finalize(pScan);
    return rc;
  }

  rc = columnarPrepare(db, &pChunk, 0, "SELECT rid_min,rid_max," " (nonnull_count=row_count AND min>=?2 AND max<=?3)" " FROM \"%w\".\"%w__columnar_chunk\"" " WHERE col=?1 AND min IS NOT NULL AND max>=?2 AND min<=?3" " ORDER BY chunk", pRef->zDb, pRef->zTab);
  if( rc!=SQLITE_OK ) return rc;
  sqlite3_bind_int(pChunk, 1, iFilterCol);
  sqlite3_bind_value(pChunk, 2, pLo);
  sqlite3_bind_value(pChunk, 3, pHi);

  rc = columnarGroupWhereOpenScan(db, &pScan, pRef, iKeyCol, iValCol, iFilterCol, 1);
  if( rc!=SQLITE_OK ) goto group_where_done;
  rc = columnarGroupWhereOpenFullScan(db, &pFullScan, pRef, iKeyCol, iValCol);
  if( rc!=SQLITE_OK ) goto group_where_done;
  while( (rc = sqlite3_step(pChunk))==SQLITE_ROW ){
    if( sqlite3_column_int(pChunk, 2) ){
      rc = columnarGroupWhereRunFullScan(pFullScan, sqlite3_column_int64(pChunk, 0), sqlite3_column_int64(pChunk, 1), pCur, pAgg);
    }else{
      rc = columnarGroupWhereRunScan(pScan, 1, sqlite3_column_int64(pChunk, 0), sqlite3_column_int64(pChunk, 1), pLo, pHi, pCur, pAgg);
    }
    if( rc!=SQLITE_OK ) goto group_where_done;
  }
  if( rc==SQLITE_DONE ) rc = SQLITE_OK;

group_where_done:
  sqlite3_finalize(pChunk);
  sqlite3_finalize(pScan);
  sqlite3_finalize(pFullScan);
  return rc;
}

static int columnarGroupWhereFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  ColumnarGroupVtab *pTab = (ColumnarGroupVtab*)cur->pVtab;
  const ColumnarGroupAgg *pAgg = pTab->pAgg;
  ColumnarTableRef sRef = {0, 0};
  sqlite3_value *pLo = 0;
  sqlite3_value *pHi = 0;
  int iKeyCol = -1;
  int iValCol = -1;
  int iFilterCol = -1;
  int aCol[3];
  int rc;
  char *zErr = 0;
  (void)idxStr;
  (void)idxNum;

  columnarGroupEntriesClear(pCur);
  if( argc<6 ){
    sqlite3_free(cur->pVtab->zErrMsg);
    cur->pVtab->zErrMsg = sqlite3_mprintf("%s requires table, key column, value column, filter column, low, and high", pAgg->zErrName);
    return SQLITE_ERROR;
  }
  rc = columnarParseVtabTableColumns(pTab->db, argv, &sRef, 3, aCol, &zErr);
  if( rc!=SQLITE_OK ) goto group_where_error;
  iKeyCol = aCol[0];
  iValCol = aCol[1];
  iFilterCol = aCol[2];
  pLo = argv[4];
  pHi = argv[5];

  rc = columnarGroupBuildWhereRange(pCur, pTab->db, &sRef, iKeyCol, iValCol, iFilterCol, pLo, pHi, pAgg);
  if( rc!=SQLITE_OK ) goto group_where_error;
  columnarGroupAdvanceToUsed(pCur);
  columnarClearTableRef(&sRef);
  return SQLITE_OK;

group_where_error:
  sqlite3_free(cur->pVtab->zErrMsg);
  cur->pVtab->zErrMsg = sqlite3_mprintf("%s", zErr ? zErr : sqlite3_errmsg(pTab->db));
  sqlite3_free(zErr);
  columnarClearTableRef(&sRef);
  return rc;
}

static int columnarGroupNext(sqlite3_vtab_cursor *cur){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  pCur->iEntry++;
  pCur->iRowid++;
  columnarGroupAdvanceToUsed(pCur);
  return SQLITE_OK;
}

static int columnarGroupEof(sqlite3_vtab_cursor *cur){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  return pCur->iEntry>=pCur->nAlloc;
}

static int columnarGroupColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  ColumnarGroupVtab *pTab = (ColumnarGroupVtab*)cur->pVtab;
  const ColumnarGroupAgg *pAgg = pTab->pAgg;
  ColumnarGroupEntry *pEntry;
  if( pCur->iEntry>=pCur->nAlloc ){
    sqlite3_result_null(ctx);
    return SQLITE_OK;
  }
  pEntry = &pCur->aEntry[pCur->iEntry];
  if( i==0 ){
    columnarValueResult(ctx, &pEntry->sKey);
    return SQLITE_OK;
  }
  switch( pAgg->eOp ){
    case COLUMNAR_GROUP_SUM:
      if( i==1 ) columnarSumResult(ctx, &pEntry->sSum);
      else sqlite3_result_null(ctx);
      break;
    case COLUMNAR_GROUP_AVG:
      if( i==1 ) columnarAvgResult(ctx, &pEntry->sSum);
      else sqlite3_result_null(ctx);
      break;
    case COLUMNAR_GROUP_COUNT:
      if( i==1 ) sqlite3_result_int64(ctx, pEntry->nCount);
      else sqlite3_result_null(ctx);
      break;
    case COLUMNAR_GROUP_SUM_AVG_COUNT:
      if( i==1 ){
        columnarSumResult(ctx, &pEntry->sSum);
      }else if( i==2 ){
        columnarAvgResult(ctx, &pEntry->sSum);
      }else if( i==3 ){
        sqlite3_result_int64(ctx, pEntry->nCount);
      }else{
        sqlite3_result_null(ctx);
      }
      break;
    case COLUMNAR_GROUP_MIN:
      if( i==1 ) columnarValueResult(ctx, &pEntry->sMin);
      else sqlite3_result_null(ctx);
      break;
    case COLUMNAR_GROUP_MAX:
      if( i==1 ) columnarValueResult(ctx, &pEntry->sMax);
      else sqlite3_result_null(ctx);
      break;
    case COLUMNAR_GROUP_MIN_MAX_COUNT:
      if( i==1 ) columnarValueResult(ctx, &pEntry->sMin);
      else if( i==2 ) columnarValueResult(ctx, &pEntry->sMax);
      else if( i==3 ) sqlite3_result_int64(ctx, pEntry->nCount);
      else sqlite3_result_null(ctx);
      break;
    case COLUMNAR_GROUP_RANGE:
      if( i==1 ){
        if( pEntry->sMax.eType!=0 && pEntry->sMin.eType!=0 ){
          /* max-min, with the same numeric coercion SQL arithmetic applies. */
          sqlite3_result_double(ctx, pEntry->sMax.rNum - pEntry->sMin.rNum);
        }else{
          sqlite3_result_null(ctx);
        }
      }else if( i==2 ){
        columnarValueResult(ctx, &pEntry->sMax);
      }else if( i==3 ){
        columnarValueResult(ctx, &pEntry->sMin);
      }else if( i==4 ){
        sqlite3_result_int64(ctx, pEntry->nCount);
      }else{
        sqlite3_result_null(ctx);
      }
      break;
    default:
      sqlite3_result_null(ctx);
      break;
  }
  return SQLITE_OK;
}

static int columnarGroupRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  ColumnarGroupCursor *pCur = (ColumnarGroupCursor*)cur;
  *pRowid = pCur->iRowid;
  return SQLITE_OK;
}

static void columnarVersionFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv){
  (void)argc;
  (void)argv;
  sqlite3_result_text(ctx, COLUMNAR_VERSION, -1, SQLITE_STATIC);
}

// PRAGMA MARK: - Extension Registration -

SQLITE_PRIVATE int sqlite3ColumnarRegister(sqlite3 *db){
  int rc;
  rc = sqlite3_create_module(db, "columnar", &columnarModule, 0);
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_version", 0, SQLITE_UTF8|SQLITE_DETERMINISTIC, 0, columnarVersionFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_sum", -1, SQLITE_UTF8, (void*)&columnarSumOp, columnarAnalyticFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_avg", -1, SQLITE_UTF8, (void*)&columnarAvgOp, columnarAnalyticFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_count", -1, SQLITE_UTF8, (void*)&columnarCountOp, columnarAnalyticFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_analyze", -1, SQLITE_UTF8, 0, columnarAnalyzeFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_sum_where_range", -1, SQLITE_UTF8, (void*)&columnarRangeSumOp, columnarRangeAnalyticFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_avg_where_range", -1, SQLITE_UTF8, (void*)&columnarRangeAvgOp, columnarRangeAnalyticFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_function(db, "columnar_count_where_range", -1, SQLITE_UTF8, (void*)&columnarRangeCountOp, columnarRangeAnalyticFunc, 0, 0);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_sum", &columnarGroupModule, (void*)&columnarGroupSumAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_avg", &columnarGroupModule, (void*)&columnarGroupAvgAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_count", &columnarGroupModule, (void*)&columnarGroupCountAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_sum_avg_count", &columnarGroupModule, (void*)&columnarGroupSumAvgCountAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_min", &columnarGroupModule, (void*)&columnarGroupMinAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_max", &columnarGroupModule, (void*)&columnarGroupMaxAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_min_max_count", &columnarGroupModule, (void*)&columnarGroupMinMaxCountAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_range", &columnarGroupModule, (void*)&columnarGroupRangeAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_sum_where_range", &columnarGroupWhereModule, (void*)&columnarGroupSumWhereAgg);
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_create_module(db, "columnar_group_sum_avg_count_where_range", &columnarGroupWhereModule, (void*)&columnarGroupSumAvgCountWhereAgg);
  }
  return rc;
}

static sqlite3_module columnarModule = {
  3,                         /* iVersion */
  columnarCreate,            /* xCreate */
  columnarConnect,           /* xConnect */
  columnarBestIndex,         /* xBestIndex */
  columnarDisconnect,        /* xDisconnect */
  columnarDestroy,           /* xDestroy */
  columnarOpen,              /* xOpen */
  columnarClose,             /* xClose */
  columnarFilter,            /* xFilter */
  columnarNext,              /* xNext */
  columnarEof,               /* xEof */
  columnarColumn,            /* xColumn */
  columnarRowid,             /* xRowid */
  columnarUpdate,            /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindFunction */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  columnarShadowName,        /* xShadowName */
  0                          /* xIntegrity */
};

static sqlite3_module columnarGroupModule = {
  3,                         /* iVersion */
  0,                         /* xCreate */
  columnarGroupConnect,      /* xConnect */
  columnarGroupBestIndex,    /* xBestIndex */
  columnarGroupDisconnect,   /* xDisconnect */
  0,                         /* xDestroy */
  columnarGroupOpen,         /* xOpen */
  columnarGroupClose,        /* xClose */
  columnarGroupFilter,       /* xFilter */
  columnarGroupNext,         /* xNext */
  columnarGroupEof,          /* xEof */
  columnarGroupColumn,       /* xColumn */
  columnarGroupRowid,        /* xRowid */
  0,                         /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindFunction */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  0,                         /* xShadowName */
  0                          /* xIntegrity */
};

static sqlite3_module columnarGroupWhereModule = {
  3,                         /* iVersion */
  0,                         /* xCreate */
  columnarGroupConnect,      /* xConnect */
  columnarGroupWhereBestIndex, /* xBestIndex */
  columnarGroupDisconnect,   /* xDisconnect */
  0,                         /* xDestroy */
  columnarGroupOpen,         /* xOpen */
  columnarGroupClose,        /* xClose */
  columnarGroupWhereFilter,  /* xFilter */
  columnarGroupNext,         /* xNext */
  columnarGroupEof,          /* xEof */
  columnarGroupColumn,       /* xColumn */
  columnarGroupRowid,        /* xRowid */
  0,                         /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindFunction */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
  0,                         /* xShadowName */
  0                          /* xIntegrity */
};

#endif /* !defined(SQLITE_OMIT_VIRTUALTABLE) */

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_columnar_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
#ifndef SQLITE_OMIT_VIRTUALTABLE
  (void)pzErrMsg;
  rc = sqlite3ColumnarRegister(db);
#else
  (void)db;
  *pzErrMsg = sqlite3_mprintf("columnar requires virtual table support");
  rc = SQLITE_ERROR;
#endif
  return rc;
}
