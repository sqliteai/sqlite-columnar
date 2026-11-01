.bail on
.load ./columnar

SELECT 'version', columnar_version();

CREATE TABLE notes__columnar_backup(x);
INSERT INTO notes__columnar_backup VALUES(1);
SELECT 'shadow-name-nonmatch', count(*) FROM notes__columnar_backup;
DROP TABLE notes__columnar_backup;

CREATE VIRTUAL TABLE "base__columnar_backup" USING columnar(a INTEGER);
INSERT INTO "base__columnar_backup" VALUES(1),(2);
SELECT 'shadow-name-last-marker', columnar_analyze('base__columnar_backup'),
       columnar_sum('base__columnar_backup','a');
DROP TABLE "base__columnar_backup";

CREATE VIRTUAL TABLE c USING columnar(a INTEGER, b TEXT, c REAL);
INSERT INTO c VALUES(1,'one',1.5),(2,'two',2.5),(3,NULL,3.5);

UPDATE c SET b='TWO', c=20.25 WHERE rowid=2;
DELETE FROM c WHERE rowid=1;
INSERT INTO c(rowid,a,b,c) VALUES(10,10,'ten',10.5);

SELECT 'agg', sum(a), count(b), round(avg(c),2) FROM c;
SELECT 'analyze', columnar_analyze('c');
SELECT 'chunk-count', count(*) FROM c__columnar_chunk;
SELECT 'meta-after-analyze', k, v FROM c__columnar_meta
 WHERE k IN ('row_count','chunk_count','dirty_count','stats_valid')
 ORDER BY k;
SELECT 'noop-analyze', columnar_analyze('c');
SELECT 'specialized', columnar_sum('c','a'), columnar_count('c','b'),
       round(columnar_avg('c','c'),2);
SELECT 'range-specialized',
       round(columnar_sum_where_range('c','c','a',2,10),2),
       round(columnar_avg_where_range('c','c','a',2,10),2),
       columnar_count_where_range('c','c','a',2,10);
SELECT 'groupsac', k, round("sum",2), round("avg",2), "count"
  FROM columnar_group_sum_avg_count('c','a','c') ORDER BY k;
SELECT 'group-range-sum', k, round("sum",2)
  FROM columnar_group_sum_where_range('c','b','c','a',2,10)
 ORDER BY k;
SELECT 'group-range-sac', k, round("sum",2), round("avg",2), "count"
  FROM columnar_group_sum_avg_count_where_range('c','b','c','a',2,10)
 ORDER BY k;
INSERT INTO c(rowid,a,b,c) VALUES(70000,70,'later',70.5);
SELECT 'dirty-before-analyze', count(*) FROM c__columnar_dirty;
SELECT 'dirty-specialized-before-analyze', columnar_sum('c','a'), columnar_count('c'),
       round(columnar_sum_where_range('c','c','a',70,70),2);
SELECT 'incremental-analyze', columnar_analyze('c');
SELECT 'dirty-after-analyze', count(*) FROM c__columnar_dirty;
SELECT 'meta-after-incremental', k, v FROM c__columnar_meta
 WHERE k IN ('row_count','chunk_count','dirty_count','stats_valid')
 ORDER BY k;
SELECT 'chunk-count-after-incremental', count(*) FROM c__columnar_chunk;
SELECT 'incremental-specialized', columnar_sum('c','a'), columnar_count('c'),
       round(columnar_avg('c','c'),2);
DROP TABLE c;

CREATE VIRTUAL TABLE cr USING columnar(a INTEGER);
INSERT INTO cr(rowid,a) VALUES(1,10),(2,20);
SELECT 'replace-analyze', columnar_analyze('cr');
UPDATE OR REPLACE cr SET rowid=2, a=11 WHERE rowid=1;
SELECT 'replace-count-before-analyze', count(*), columnar_count('cr'),
       (SELECT v FROM cr__columnar_meta WHERE k='row_count')
  FROM cr;
SELECT 'replace-incremental-analyze', columnar_analyze('cr');
SELECT 'replace-count-after-analyze', count(*), columnar_count('cr'),
       (SELECT v FROM cr__columnar_meta WHERE k='row_count')
  FROM cr;
DROP TABLE cr;

CREATE VIRTUAL TABLE ck USING columnar(k, v REAL);
INSERT INTO ck VALUES
  (NULL,1.0),(NULL,2.0),
  ('a',3.0),('a',4.0),('b',5.0),
  (x'6162',6.0),(x'6162',7.0),
  (1,8.0),(1.0,9.0);
SELECT 'mixedkey', typeof(k), quote(k), round("sum",2), round("avg",2), "count"
  FROM columnar_group_sum_avg_count('ck','k','v')
 ORDER BY typeof(k), quote(k);
SELECT 'mixedcount', typeof(k), quote(k), "count"
  FROM columnar_group_count('ck','k')
 ORDER BY typeof(k), quote(k);
DROP TABLE ck;

ATTACH ':memory:' AS aux;
CREATE VIRTUAL TABLE aux.ca USING columnar(a INTEGER, b TEXT, c REAL);
INSERT INTO aux.ca VALUES
  (1,'north',10.0),
  (2,'north',20.0),
  (3,'south',30.0),
  (4,'south',NULL);
SELECT 'attached-analyze', columnar_analyze('aux.ca');
SELECT 'attached-specialized',
       columnar_sum('aux.ca','a'),
       round(columnar_avg('aux.ca','c'),2),
       columnar_count('aux.ca'),
       columnar_count('aux.ca','c');
SELECT 'attached-range',
       round(columnar_sum_where_range('aux.ca','c','a',2,4),2),
       round(columnar_avg_where_range('aux.ca','c','a',2,4),2),
       columnar_count_where_range('aux.ca','c','a',2,4);
SELECT 'attached-group', k, round("sum",2), round("avg",2), "count"
  FROM columnar_group_sum_avg_count('aux.ca','b','c')
 ORDER BY k;
SELECT 'attached-group-range', k, round("sum",2)
  FROM columnar_group_sum_where_range('aux.ca','b','c','a',2,4)
 ORDER BY k;
DETACH aux;

-- Regression tests: each block below compares a columnar helper against the
-- equivalent ordinary SQLite query.  Every 'ok' row must read ok.

-- Declared column affinity is applied to stored values.
CREATE VIRTUAL TABLE aff USING columnar(a INTEGER, b TEXT, c REAL, d);
CREATE TABLE affref(a INTEGER, b TEXT, c REAL, d);
INSERT INTO aff    VALUES('5','5','5','5'),(10,10,10,10),('7',7,'7',7);
INSERT INTO affref VALUES('5','5','5','5'),(10,10,10,10),('7',7,'7',7);
SELECT 'affinity-typeof', CASE WHEN (SELECT group_concat(typeof(a)||typeof(b)||typeof(c)||typeof(d))
                                      FROM (SELECT * FROM aff ORDER BY rowid))
                             = (SELECT group_concat(typeof(a)||typeof(b)||typeof(c)||typeof(d))
                                      FROM (SELECT * FROM affref ORDER BY rowid))
                        THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'affinity-order', CASE WHEN (SELECT group_concat(quote(a)) FROM (SELECT a FROM aff ORDER BY a))
                                 = (SELECT group_concat(quote(a)) FROM (SELECT a FROM affref ORDER BY a))
                            THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'affinity-maxmin', CASE WHEN quote((SELECT max(a) FROM aff))=quote((SELECT max(a) FROM affref))
                                AND quote((SELECT min(a) FROM aff))=quote((SELECT min(a) FROM affref))
                           THEN 'ok' ELSE 'MISMATCH' END;

-- Range helpers apply the filter column's affinity to their bounds, both on
-- the row scan and through the chunk zone map.
CREATE VIRTUAL TABLE rng USING columnar(v REAL, f INTEGER);
INSERT INTO rng VALUES(1.0,5),(2.0,15),(3.0,25);
SELECT 'range-text-bounds-unanalyzed',
       CASE WHEN columnar_count_where_range('rng','v','f','10','20')
               = (SELECT count(v) FROM rng WHERE f BETWEEN '10' AND '20')
       THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'range-analyze', columnar_analyze('rng');
SELECT 'range-text-bounds-analyzed',
       CASE WHEN columnar_count_where_range('rng','v','f','10','20')
               = (SELECT count(v) FROM rng WHERE f BETWEEN '10' AND '20')
        AND quote(columnar_sum_where_range('rng','v','f','10','20'))
          = quote((SELECT sum(v) FROM rng WHERE f BETWEEN '10' AND '20'))
       THEN 'ok' ELSE 'MISMATCH' END;

-- Empty text and zero-length blob keys stay distinct from NULL.
CREATE VIRTUAL TABLE gk USING columnar(k TEXT, v REAL);
INSERT INTO gk VALUES('',1.0),('',2.0),(NULL,10.0),(x'',5.0),('a',100.0);
SELECT 'group-empty-keys', typeof(k), quote(k), "sum"
  FROM columnar_group_sum('gk','k','v') ORDER BY "sum";

-- Integer sums keep full precision and integer storage class.
CREATE VIRTUAL TABLE bigi USING columnar(n INTEGER, g TEXT);
INSERT INTO bigi VALUES(9007199254740993,'a'),(1,'a'),(2,'a');
SELECT 'sum-int-exact',
       CASE WHEN quote(columnar_sum('bigi','n'))=quote((SELECT sum(n) FROM bigi))
        AND (SELECT quote("sum") FROM columnar_group_sum('bigi','g','n'))
          = (SELECT quote(sum(n)) FROM bigi GROUP BY g)
       THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'sum-int-analyze', columnar_analyze('bigi');
SELECT 'sum-int-exact-analyzed',
       CASE WHEN quote(columnar_sum('bigi','n'))=quote((SELECT sum(n) FROM bigi))
       THEN 'ok' ELSE 'MISMATCH' END;

-- Grouped min/max follow SQLite's sort order and keep storage classes.
CREATE VIRTUAL TABLE gm USING columnar(g TEXT, s TEXT, t TEXT);
INSERT INTO gm VALUES('a','pear','x'),('a','apple','z'),('a','zebra','y');
SELECT 'group-min-max-text',
       CASE WHEN (SELECT quote("min")||'/'||quote("max")
                    FROM columnar_group_min_max_count('gm','g','s'))
               = (SELECT quote(min(s))||'/'||quote(max(s)) FROM gm GROUP BY g)
        AND (SELECT quote("min") FROM columnar_group_min('gm','g','s'))
          = (SELECT quote(min(s)) FROM gm GROUP BY g)
        AND (SELECT quote("max") FROM columnar_group_max('gm','g','s'))
          = (SELECT quote(max(s)) FROM gm GROUP BY g)
       THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'group-range-text',
       CASE WHEN (SELECT quote("max")||'/'||quote("min")
                    FROM columnar_group_range('gm','g','s','t'))
               = (SELECT quote(max(s))||'/'||quote(min(t)) FROM gm GROUP BY g)
       THEN 'ok' ELSE 'MISMATCH' END;


-- Table names are case-insensitive, so the helpers must accept any spelling.
CREATE VIRTUAL TABLE MixedCase USING columnar(a INTEGER);
INSERT INTO MixedCase VALUES(1),(2),(3);
SELECT 'case-analyze-exact', columnar_analyze('MixedCase');
SELECT 'case-analyze-upper', columnar_analyze('MIXEDCASE');
SELECT 'case-analyze-lower', columnar_analyze('mixedcase');
SELECT 'case-sum', CASE WHEN quote(columnar_sum('MIXEDCASE','a'))=quote((SELECT sum(a) FROM MixedCase))
                   THEN 'ok' ELSE 'MISMATCH' END;

-- A 1.0.0 database declares the chunk/stat sum columns REAL, which cannot hold
-- an exact integer total.  Recreating that schema here stands in for such a
-- database: the next analyze must replace it and recover full precision.
CREATE VIRTUAL TABLE legacy USING columnar(n INTEGER);
INSERT INTO legacy VALUES(9007199254740993),(1),(2);
SELECT 'legacy-seed-analyze', columnar_analyze('legacy');
DROP TABLE legacy__columnar_chunk;
DROP TABLE legacy__columnar_stat;
CREATE TABLE legacy__columnar_chunk(col INTEGER NOT NULL, chunk INTEGER NOT NULL,
  rid_min INTEGER NOT NULL, rid_max INTEGER NOT NULL, row_count INTEGER NOT NULL,
  nonnull_count INTEGER NOT NULL, sum REAL, min, max, PRIMARY KEY(col,chunk));
CREATE TABLE legacy__columnar_stat(col INTEGER PRIMARY KEY, row_count INTEGER NOT NULL,
  nonnull_count INTEGER NOT NULL, sum REAL, min, max);
UPDATE legacy__columnar_meta SET v=0 WHERE k IN ('chunk_count','stats_valid');
SELECT 'legacy-sum-decl-before', (SELECT type FROM pragma_table_info('legacy__columnar_chunk') WHERE name='sum');
SELECT 'legacy-migrating-analyze', columnar_analyze('legacy');
SELECT 'legacy-sum-decl-after', '['||(SELECT type FROM pragma_table_info('legacy__columnar_chunk') WHERE name='sum')||']';
SELECT 'legacy-sum-exact', CASE WHEN quote(columnar_sum('legacy','n'))=quote((SELECT sum(n) FROM legacy))
                           THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'legacy-analyze-idempotent', columnar_analyze('legacy');

-- A table constraint among the module arguments declares no column. Counting
-- it as one used to make the write path read past the end of xUpdate's
-- argument vector. The shadow column count must match the declared count.
CREATE VIRTUAL TABLE tc1 USING columnar(a INTEGER, CHECK(a>0));
CREATE VIRTUAL TABLE tc2 USING columnar(a INTEGER, b TEXT, PRIMARY KEY(a));
CREATE VIRTUAL TABLE tc3 USING columnar(a INTEGER, CONSTRAINT u UNIQUE(a));
CREATE VIRTUAL TABLE tc4 USING columnar(a INTEGER, b TEXT, FOREIGN KEY(a) REFERENCES tc1(a));
CREATE VIRTUAL TABLE tc5 USING columnar("check" INTEGER, [primary] TEXT);
INSERT INTO tc1 VALUES(1);
INSERT INTO tc2 VALUES(2,'b');
INSERT INTO tc3 VALUES(3);
INSERT INTO tc4 VALUES(4,'d');
INSERT INTO tc5 VALUES(5,'e');
SELECT 'constraint-arg-shadow-count', CASE WHEN
      (SELECT count(*) FROM pragma_table_info('tc1'))
    = (SELECT count(*) FROM sqlite_master WHERE name GLOB 'tc1__columnar_c[0-9]*')
  AND (SELECT count(*) FROM pragma_table_info('tc2'))
    = (SELECT count(*) FROM sqlite_master WHERE name GLOB 'tc2__columnar_c[0-9]*')
  AND (SELECT count(*) FROM pragma_table_info('tc3'))
    = (SELECT count(*) FROM sqlite_master WHERE name GLOB 'tc3__columnar_c[0-9]*')
  AND (SELECT count(*) FROM pragma_table_info('tc4'))
    = (SELECT count(*) FROM sqlite_master WHERE name GLOB 'tc4__columnar_c[0-9]*')
  THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'constraint-arg-roundtrip', CASE WHEN
      (SELECT a FROM tc1)=1 AND (SELECT a||b FROM tc2)='2b'
  AND (SELECT a FROM tc3)=3 AND (SELECT a||b FROM tc4)='4d'
  THEN 'ok' ELSE 'MISMATCH' END;
-- A quoted keyword is a column name, not a constraint.
SELECT 'quoted-keyword-is-column', CASE WHEN
      (SELECT count(*) FROM pragma_table_info('tc5'))=2
  AND (SELECT "check"||[primary] FROM tc5)='5e'
  THEN 'ok' ELSE 'MISMATCH' END;
-- A table of nothing but constraints declares no column and must be refused.
SELECT 'no-columns-refused', CASE WHEN NOT EXISTS(
    SELECT 1 FROM sqlite_master WHERE name='tc_bad') THEN 'ok' ELSE 'MISMATCH' END;

-- The zone map's min/max columns are typeless, so a bound whose type does not
-- match its filter column cannot be compared against them. The answer must
-- still match ordinary SQL; only the chunk-skipping optimization is skipped.
CREATE VIRTUAL TABLE zb USING columnar(v REAL, f INTEGER, t TEXT);
INSERT INTO zb VALUES(1.0,5,'aa'),(2.0,15,'mm'),(3.0,25,'zz');
SELECT 'zone-analyze', columnar_analyze('zb');
SELECT 'zone-text-bound-on-numeric-col', CASE WHEN
      columnar_count_where_range('zb','v','f','10','20')
    = (SELECT count(v) FROM zb WHERE f BETWEEN '10' AND '20')
  AND quote(columnar_sum_where_range('zb','v','f','10','20'))
    = quote((SELECT sum(v) FROM zb WHERE f BETWEEN '10' AND '20'))
  THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'zone-numeric-bound-on-text-col', CASE WHEN
      columnar_count_where_range('zb','v','t',10,20)
    = (SELECT count(v) FROM zb WHERE t BETWEEN 10 AND 20)
  THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'zone-matching-bound-types', CASE WHEN
      columnar_count_where_range('zb','v','f',10,20)
    = (SELECT count(v) FROM zb WHERE f BETWEEN 10 AND 20)
  AND columnar_count_where_range('zb','v','t','a','n')
    = (SELECT count(v) FROM zb WHERE t BETWEEN 'a' AND 'n')
  THEN 'ok' ELSE 'MISMATCH' END;

-- Summing numeric text held in a column with no declared affinity gives the
-- right total with a real storage class, where sum() returns an integer. A
-- column with a numeric affinity converts such text as it is stored, so this
-- only arises for an untyped column.
CREATE VIRTUAL TABLE st USING columnar(a, b INTEGER);
INSERT INTO st VALUES('10',' 10'),(5,5);
SELECT 'untyped-numeric-text-value', CASE WHEN
      columnar_sum('st','a') = (SELECT sum(a) FROM st)
  THEN 'ok' ELSE 'MISMATCH' END;
SELECT 'typed-col-converts-on-store', CASE WHEN
      quote(columnar_sum('st','b')) = quote((SELECT sum(b) FROM st))
  THEN 'ok' ELSE 'MISMATCH' END;
