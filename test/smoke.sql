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
