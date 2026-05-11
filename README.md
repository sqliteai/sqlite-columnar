# sqlite-columnar

`sqlite-columnar` brings column-oriented analytics to SQLite as a self-contained
loadable extension. It lets applications keep the operational simplicity of
SQLite while adding a storage and execution path built for analytical scans,
aggregations, and grouped summaries over wide datasets.

It does not patch SQLite's pager, btree, parser, VDBE, or shell. Build it as a
normal extension, load it into SQLite, and create columnar virtual tables for
the parts of your application that behave more like analytics than OLTP.

## Why Columnar SQLite?

Traditional SQLite tables are row-oriented, which is excellent for point
lookups, small updates, and transactional application state. Analytical
workloads are different: they often read a few columns across many rows, compute
aggregates, group by dimensions, and filter by ranges. In those cases, reading
entire rows means paying I/O and CPU cost for data the query never uses.

`sqlite-columnar` stores each column separately, tracks chunk-level metadata,
and provides specialized aggregate helpers that avoid generic row
materialization for common analytical queries.

## Performance Highlights

On the included 10 million row variance benchmark, `sqlite-columnar` shows
large median speedups over standard row-oriented SQLite for operations that
benefit from columnar layout and precomputed metadata:

- `sum(v1)` with `columnar_sum`: **130,583x faster**
- `avg(v3)` with `columnar_avg`: **129,317x faster**
- `count(v1)` with `columnar_count`: **125,194x faster**
- grouped `sum` by dimension: **6.04x faster**
- grouped `count` by dimension: **14.13x faster**
- grouped `sum/avg/count` by dimension: **6.42x faster**
- clustered range filter on `ts`: **248.89x faster**
- clustered range filter plus grouped `sum/avg/count`: **273.20x faster**

These numbers are workload-specific. They are strongest when queries scan a
small subset of columns, use aggregate metadata, group over low-cardinality
dimensions, or filter on clustered/range-friendly columns. See
[BENCHMARK.md](BENCHMARK.md) for the full dataset, commands, timings, and
interpretation.

## Common Use Cases

`sqlite-columnar` is useful when an embedded application needs analytical
queries without moving data into a separate database server.

Good fits include:

- embedded dashboards over local event, telemetry, or product analytics data
- time-series rollups where queries filter by timestamp ranges
- IoT and edge analytics over wide sensor records
- desktop or mobile apps with local reporting and summary views
- feature stores or ML preprocessing jobs that scan a few feature columns at a
  time
- audit logs and observability data where users aggregate by service, region,
  status, or time bucket
- SaaS tenant-local analytics where a single-file SQLite database is still the
  preferred deployment model
- ETL validation workloads that repeatedly compute counts, sums, min/max, and
  grouped quality checks

Row-oriented SQLite remains the better default for highly transactional
workloads, point lookups, and frequent single-row updates. `sqlite-columnar` is
intended for the analytical tables in the same application.

## How It Works

Each columnar virtual table owns shadow tables for rowids, column values,
global stats, chunk zone maps, dirty chunks, and table-level metadata.

`columnar_analyze()` builds the metadata used by specialized analytical
functions. After the initial bootstrap, analyze is incremental: inserts,
updates, and deletes mark touched chunks dirty, and later analyze calls rebuild
only those chunks. If metadata says stats are valid and there are no dirty
chunks, analyze returns immediately.

Range-filtered helpers use chunk min/max summaries to skip rowid ranges that
cannot match a filter. Grouped helpers perform hash aggregation in C over only
the required column shadow tables.

## Build

From this directory:

```sh
make
```

By default the build uses the bundled `sqlite/` directory for `sqlite3ext.h`.
To build against a different SQLite checkout or amalgamation directory, pass
`SQLITE_SRC`:

```sh
make SQLITE_SRC=/path/to/sqlite
```

This produces `columnar.dylib` on macOS or `columnar.so` on Linux.

## Prebuilt Binaries

Tagged releases build loadable extension binaries for Linux, Linux musl,
macOS, Windows, Android, iOS, and iOS Simulator. macOS release assets are
Developer ID signed and notarized ZIP archives. Other platforms are published
as release archives. Each asset contains the platform binary plus `README.md`,
`API.md`, `BENCHMARK.md`, and `GIT_COMMIT`. Release assets also include
`SHA256SUMS` for archive verification.

## Quick Start

```sql
.load ./columnar

CREATE VIRTUAL TABLE sales USING columnar(
  id INTEGER,
  region TEXT,
  amount REAL
);

INSERT INTO sales VALUES
  (1, 'north', 10.0),
  (2, 'north', 20.0),
  (3, 'south', 5.0);

SELECT columnar_analyze('sales');
SELECT columnar_sum('sales', 'amount');

SELECT k, "sum", "avg", "count"
  FROM columnar_group_sum_avg_count('sales', 'region', 'amount')
 ORDER BY k;
```

See [API.md](API.md) for the complete SQL API reference with examples for every
function and table-valued helper.

## Benchmarks

The benchmark suite is built against the bundled `sqlite/sqlite3.c` by default:

```sh
make benchmarks
build/columnar-analytics-bench ./columnar 10000000 256
```

[BENCHMARK.md](BENCHMARK.md) documents the main analytical benchmark: schema,
dataset shape, load/analyze costs, storage size, query timings, speedups, and
the interpretation of where columnar storage helps most. Use it as the baseline
for evaluating changes and for understanding which query patterns benefit from
this extension.

Use `make smoke-bench` for small correctness-oriented benchmark runs.

Use `make variance-bench` to run the repeatable performance-variance suite. It
builds multiple deterministic datasets, warms each query once, repeats each
measurement, verifies row-store/columnar result hashes, and reports median and
p95 timings:

```sh
make variance-bench VARIANCE_REPEATS=9 \
  VARIANCE_DATASETS="small:10000:64 medium:50000:128 wide:50000:512"
```

## Test

```sh
make test SQLITE3=/path/to/sqlite3
```

The test target runs the SQL smoke suite plus a native robustness suite. The
robustness suite checks rollback and savepoint behavior, simulated process
death during an uncommitted transaction, unusual table/column names and mixed
SQLite storage classes, and automatic result equivalence between each
specialized columnar query helper and the matching ordinary SQLite query.

## Part of the SQLite AI Ecosystem

This project is part of the [**SQLite AI**](https://sqlite.ai) ecosystem, a collection of extensions that bring modern AI capabilities to the world’s most widely deployed database. The goal is to make SQLite the default data and inference engine for Edge AI applications.

Other projects in the ecosystem include:

| Extension | Description |
|-----------|-------------|
| **[SQLite-Sync](https://github.com/sqliteai/sqlite-sync)** | Local-first CRDT-based synchronization for seamless, conflict-free data sync and real-time collaboration across devices.
| **[SQLite-AI](https://github.com/sqliteai/sqlite-ai)** | On-device inference and embedding generation |
| **[SQLite-Memory](https://github.com/sqliteai/sqlite-memory)** | Markdown-based AI agent memory with semantic search |
| **[SQLite-Vector](https://github.com/sqliteai/sqlite-vector)** | Vector search for embeddings stored as BLOBs |
| **[SQLite-Agent](https://github.com/sqliteai/sqlite-agent)** | Run autonomous AI agents from within SQLite |
| **[SQLite-MCP](https://github.com/sqliteai/sqlite-mcp)** | Connect SQLite databases to MCP servers |
| **[SQLite-JS](https://github.com/sqliteai/sqlite-js)** | Custom SQLite functions in JavaScript |
| **[Liteparser](https://github.com/sqliteai/liteparser)** | Fully compliant SQLite SQL parser |

## License

This project is licensed under the [Elastic License 2.0](./LICENSE.md). For production or managed service use, [contact SQLite Cloud, Inc](mailto:info@sqlitecloud.io) for a commercial license.
