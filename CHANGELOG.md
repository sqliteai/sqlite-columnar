# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

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
