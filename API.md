# sqlite-columnar API

This document lists the SQL API exposed by the `columnar` loadable extension.

Examples assume the extension is loaded and this table exists:

```sql
.load ./columnar

CREATE VIRTUAL TABLE sales USING columnar(
  id INTEGER,
  region TEXT,
  category TEXT,
  amount REAL,
  cost REAL
);

INSERT INTO sales VALUES
  (1, 'north', 'hardware', 10.0, 4.0),
  (2, 'north', 'software', 20.0, 8.0),
  (3, 'south', 'hardware', 5.0, 2.0);
```

Every API argument named `table` accepts either `table` for the main database
or `db.table` for an attached database schema.

## Virtual Table

### `CREATE VIRTUAL TABLE ... USING columnar(...)`

Creates a column-oriented virtual table. Each declared column is stored in its
own shadow table, with separate rowid, stats, chunk, dirty, and metadata shadow
tables.

Arguments are column definitions. A table constraint may be given and is
accepted for compatibility, but it declares no column and is not enforced,
since SQLite does not apply `CHECK`, `UNIQUE`, `PRIMARY KEY` or foreign key
constraints to virtual tables. At least one column is required.

```sql
CREATE VIRTUAL TABLE metrics USING columnar(
  ts INTEGER,
  host TEXT,
  cpu REAL
);
```

## Scalar Functions

### `columnar_version()`

Returns the extension version as TEXT.

```sql
SELECT columnar_version();
```

### `columnar_analyze(table)`

Builds or refreshes per-column stats and chunk zone maps for a columnar table.
The return value is the number of column/chunk entries analyzed. If metadata is
valid and no chunks are dirty, it returns `0`.

```sql
SELECT columnar_analyze('sales');
```

```sql
ATTACH 'analytics.db' AS analytics;
SELECT columnar_analyze('analytics.sales');
```

### `columnar_sum(table, column)`

Returns `sum(column)` for a columnar table. Uses analyzed stats when available,
otherwise scans the requested column.

```sql
SELECT columnar_sum('sales', 'amount');
```

```sql
SELECT columnar_sum('analytics.sales', 'amount');
```

### `columnar_avg(table, column)`

Returns `avg(column)` for a columnar table. Uses analyzed stats when available,
otherwise scans the requested column.

```sql
SELECT columnar_avg('sales', 'amount');
```

```sql
SELECT columnar_avg('analytics.sales', 'amount');
```

### `columnar_count(table)`

Returns the row count for a columnar table. Uses analyzed stats when available,
otherwise scans the rowid shadow table.

```sql
SELECT columnar_count('sales');
```

```sql
SELECT columnar_count('analytics.sales');
```

### `columnar_count(table, column)`

Returns `count(column)` for a columnar table. Uses analyzed stats when
available, otherwise scans the requested column.

```sql
SELECT columnar_count('sales', 'amount');
```

```sql
SELECT columnar_count('analytics.sales', 'amount');
```

### `columnar_sum_where_range(table, value_column, filter_column, low, high)`

Returns `sum(value_column)` for rows where `filter_column BETWEEN low AND high`.
Uses chunk zone maps to skip disjoint chunks after `columnar_analyze()`.

```sql
SELECT columnar_sum_where_range('sales', 'amount', 'id', 1, 2);
```

```sql
SELECT columnar_sum_where_range('analytics.sales', 'amount', 'id', 1, 2);
```

### `columnar_avg_where_range(table, value_column, filter_column, low, high)`

Returns `avg(value_column)` for rows where `filter_column BETWEEN low AND high`.

```sql
SELECT columnar_avg_where_range('sales', 'amount', 'id', 1, 2);
```

```sql
SELECT columnar_avg_where_range('analytics.sales', 'amount', 'id', 1, 2);
```

### `columnar_count_where_range(table, value_column, filter_column, low, high)`

Returns `count(value_column)` for rows where `filter_column BETWEEN low AND
high`.

```sql
SELECT columnar_count_where_range('sales', 'amount', 'id', 1, 2);
```

```sql
SELECT columnar_count_where_range('analytics.sales', 'amount', 'id', 1, 2);
```

## Grouped Table-Valued Functions

Grouped helpers return rows with a grouping key column named `k` plus one or
more aggregate output columns. Quote aggregate column names such as `"sum"` and
`"count"` in SQL.

### `columnar_group_sum(table, key_column, value_column)`

Returns one row per key with `k` and `"sum"`.

```sql
SELECT k, "sum"
  FROM columnar_group_sum('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_avg(table, key_column, value_column)`

Returns one row per key with `k` and `"avg"`.

```sql
SELECT k, "avg"
  FROM columnar_group_avg('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_count(table, key_column)`

Returns one row per key with `k` and `"count"`, counting rows in each group.

```sql
SELECT k, "count"
  FROM columnar_group_count('sales', 'region')
 ORDER BY k;
```

### `columnar_group_count(table, key_column, value_column)`

Returns one row per key with `k` and `"count"`, counting non-NULL values in
`value_column` for each group.

```sql
SELECT k, "count"
  FROM columnar_group_count('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_sum_avg_count(table, key_column, value_column)`

Returns one row per key with `k`, `"sum"`, `"avg"`, and `"count"` in one pass.

```sql
SELECT k, "sum", "avg", "count"
  FROM columnar_group_sum_avg_count('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_min(table, key_column, value_column)`

Returns one row per key with `k` and `"min"`.

```sql
SELECT k, "min"
  FROM columnar_group_min('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_max(table, key_column, value_column)`

Returns one row per key with `k` and `"max"`.

```sql
SELECT k, "max"
  FROM columnar_group_max('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_min_max_count(table, key_column, value_column)`

Returns one row per key with `k`, `"min"`, `"max"`, and `"count"` in one pass.

```sql
SELECT k, "min", "max", "count"
  FROM columnar_group_min_max_count('sales', 'region', 'amount')
 ORDER BY k;
```

### `columnar_group_range(table, key_column, max_column, min_column)`

Returns one row per key with `k`, `"range"`, `"max"`, `"min"`, and `"count"`.
The `"range"` column is `max(max_column) - min(min_column)`.
The `"count"` column counts rows where either `max_column` or `min_column` is
non-NULL.

```sql
SELECT k, "range", "max", "min", "count"
  FROM columnar_group_range('sales', 'region', 'amount', 'cost')
 ORDER BY k;
```

### `columnar_group_sum_where_range(table, key_column, value_column, filter_column, low, high)`

Returns grouped sums for rows where `filter_column BETWEEN low AND high`.
Uses chunk zone maps to skip disjoint chunks after `columnar_analyze()`.

```sql
SELECT k, "sum"
  FROM columnar_group_sum_where_range('sales', 'region', 'amount', 'id', 1, 2)
 ORDER BY k;
```

### `columnar_group_sum_avg_count_where_range(table, key_column, value_column, filter_column, low, high)`

Returns grouped sum, average, and count for rows where
`filter_column BETWEEN low AND high`.

```sql
SELECT k, "sum", "avg", "count"
  FROM columnar_group_sum_avg_count_where_range(
    'sales', 'region', 'amount', 'id', 1, 2
  )
 ORDER BY k;
```

## Notes

- `columnar_analyze()` creates and refreshes the stats and chunk metadata used
  by the specialized helpers. It fails without writing anything if the name
  does not refer to a columnar table. On a database written by version 1.0.0
  the first call also rebuilds the chunk and stat tables once, so that analyzed
  sums keep integer precision.
- Columns carry the affinity of their declared type, so values are stored and
  compared exactly as they would be in an ordinary table.
- Group keys preserve SQLite storage classes for `NULL`, integer, real, text,
  and blob values, including empty strings and zero-length blobs. When a key
  appears as both an integer and a real, the two are one group and either value
  may be reported as the key -- as with `GROUP BY`, which representative you
  get depends on the query plan.
- `columnar_sum()` and the grouped sums return an integer when every value
  summed is an integer, matching `sum()`. They fall back to a floating point
  result if the exact total would overflow, where `sum()` raises an error, and
  also when the column holds text that `sum()` would read as a number -- the
  total is the same, only its storage class differs. A column with a numeric
  declared type converts such text as it is stored, so this applies only to a
  column declared with no type.
- The `*_where_range()` helpers skip the chunk zone map, and scan instead, when
  a bound's storage class differs from the one its filter column stores -- for
  example a text bound against an `INTEGER` column. The result is the same;
  only the chunk-skipping optimization is given up.
- `columnar_group_min()`, `columnar_group_max()`, `columnar_group_min_max_count()`
  and the `"min"`/`"max"` columns of `columnar_group_range()` compare values in
  SQLite's own sort order and preserve their storage class, so text and blob
  columns give the same answers as `min()` and `max()`.
- Aggregates answered from `columnar_analyze()` metadata combine per-chunk
  subtotals, so a floating point sum or average may differ from a full scan in
  the last few digits.
- Shadow tables named `<table>__columnar_*` are implementation details, not
  stable public API.
