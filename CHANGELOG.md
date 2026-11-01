# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.1.1] - 2026-11-01

### Fixed

- **Fixed a crash in the range helpers and in sums over text, when loaded
  against SQLite 3.53.2 or later.** Both paths converted a value by duplicating
  it with `sqlite3_value_dup()` and applying numeric affinity to the copy with
  `sqlite3_value_numeric_type()`. That combination segfaults on newer SQLite
  builds, and a loadable extension runs against whatever version the host
  provides, so neither path may rely on it. Isolated from this extension, the
  same two calls crash on 3.53.4 and succeed on 3.53.1.
  - The chunk zone map is now skipped, in favour of a plain scan, when a bound's
    storage class differs from the one its filter column stores. Results are
    unchanged; only the chunk-skipping optimization is given up for that shape.
  - Sums over text that `sum()` would read as a number now use the floating
    point accumulator. The total is unchanged; only its storage class differs,
    and only for a column declared with no type.

## [1.1.0] - 2026-11-01

### Security

- **Fixed an out-of-bounds read on every write to a table declared with a table
  constraint.** A module argument such as `CHECK(...)`, `PRIMARY KEY(...)`,
  `UNIQUE(...)`, `CONSTRAINT ... ` or `FOREIGN KEY(...)` declares no column, but
  the column count was taken from the number of arguments. Since SQLite sizes
  `xUpdate`'s argument vector from the number of columns it declared, every
  `INSERT` or `UPDATE` read one element past the end of that vector and
  dereferenced it as an `sqlite3_value*`. Two ordinary statements were enough to
  crash the process, and the table could be declared in a database file rather
  than typed. The column count now matches what was declared, and the write path
  honours the argument count it is given.
- **Fixed a use-after-free and double free when `DROP TABLE` fails.** SQLite
  keeps its reference to a virtual table unless `xDestroy` succeeds, and
  disconnects the table itself afterwards; the table was freed regardless of the
  outcome, so the rollback of a failed `DROP` read the freed memory and freed it
  a second time. Reachable whenever dropping a shadow table fails -- an
  authorizer denial, a lock held by another statement, an I/O error or a full
  disk. The table is now freed only on success, and a failed `DROP` leaves it
  intact and usable.
- **Fixed a free of uninitialized pointers under memory pressure.** The array of
  shadow table names was zeroed only after the allocation-failure check that
  frees its contents, so a failure in any of the preceding allocations passed
  uninitialized values to `sqlite3_free()`.

### Fixed

- **Column affinity is now applied to stored values.** Each column's shadow
  table carries the affinity of the declared type, so inserting `'5'` into an
  `INTEGER` column stores the integer `5` as an ordinary table would. Without
  it `ORDER BY`, `min()`, `max()` and comparisons against other columns could
  disagree with the equivalent row-oriented query.
- **`*_where_range()` helpers now convert their bounds.** Text bounds against a
  numeric column were compared without affinity, both in the row scan and in
  the chunk zone map, so queries such as
  `columnar_count_where_range(t,'v','f','10','20')` returned `0` instead of the
  matching rows once the table had been analyzed.
- **Empty text and zero-length blob group keys are no longer reported as
  `NULL`.** They were indistinguishable from a genuine `NULL` group in the
  output of every `columnar_group_*()` helper.
- **`columnar_sum()` and the grouped sums keep integer precision.** Sums were
  accumulated as doubles and stored in a `REAL` stats column, so large integer
  totals came back altered. Floating point sums now use the same compensated
  summation as SQLite's `sum()`. The chunk and stat tables of a database
  written by 1.0.0 still declare that column `REAL`; the first
  `columnar_analyze()` call replaces both tables and rebuilds them once, after
  which analyzed sums are exact too. Both tables hold only data derived from
  the column tables, so nothing is lost. If the schema cannot be changed, the
  old tables are kept and keep behaving as before.
- **`columnar_group_min()`, `columnar_group_max()`,
  `columnar_group_min_max_count()` and `columnar_group_range()` handle
  non-numeric values.** Text and blob values were coerced to `0.0`, so the
  helpers reported `0.0` instead of the real minimum and maximum.
- **`columnar_analyze()` validates its argument before writing.** Called on a
  table that is not columnar, or on a name that does not exist, it created
  `<name>__columnar_meta`, `_chunk` and `_dirty` tables and left them behind in
  the schema before reporting an error.
- Fixed a leak of `sqlite3_vtab.zErrMsg` on two argument-count error paths in
  the grouped table-valued functions, and a leak of the converted range bounds
  when preparing the zone-map query fails.
- `DROP TABLE` now also removes a shadow column table left over from a release
  that counted table constraints as columns, so the table name can be reused.

## [1.0.0] - 2026-05-11

### Added

- **Columnar virtual table** for SQLite: `CREATE VIRTUAL TABLE ... USING columnar(...)` stores each column separately, so analytical queries read only the columns they need instead of full rows. It is a self-contained loadable extension — no patched SQLite build required.
- **`columnar_analyze(table)`** to build the metadata (global stats and per-chunk min/max zone maps) used by the specialized analytical functions. After the first run it is incremental: only chunks touched by inserts, updates, or deletes are rebuilt, and it returns immediately when nothing changed.
- **Metadata-backed aggregates**: `columnar_sum()`, `columnar_avg()`, and `columnar_count()` answer whole-table aggregates from precomputed stats instead of scanning rows.
- **Range-filtered aggregates**: `columnar_sum_where_range()`, `columnar_avg_where_range()`, and `columnar_count_where_range()` use chunk min/max summaries to skip row ranges that cannot match the filter — a large win on clustered columns such as timestamps.
- **Grouped table-valued functions** performing hash aggregation in C over just the required columns: `columnar_group_sum()`, `columnar_group_avg()`, `columnar_group_count()`, `columnar_group_min()`, `columnar_group_max()`, and the combined `columnar_group_sum_avg_count()`, `columnar_group_min_max_count()`, `columnar_group_range()`, plus the filtered variants `columnar_group_sum_where_range()` and `columnar_group_sum_avg_count_where_range()`.
- **`columnar_version()`** scalar function reporting the extension version.
- **Prebuilt release binaries** for Linux, Linux musl, macOS, Windows, Android, iOS, and iOS Simulator. macOS assets are Developer ID signed and notarized; every archive ships the platform binary, documentation, `GIT_COMMIT`, and a `SHA256SUMS` file for verification.
- **Documentation**: [API.md](API.md) with the complete SQL reference and examples for every function, and [BENCHMARK.md](BENCHMARK.md) with the dataset, commands, timings, and guidance on which query patterns benefit from columnar storage.
