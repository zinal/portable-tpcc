# Shared Libraries and DBMS Adapter API

Status: architecture draft, companion to [specification.md](specification.md).

This document describes the C++ library boundaries inside `tpcc/` and the
contract each `tpcc/dbms/<name>` adapter MUST implement. It is normative for
adapter authors. Orchestration (`mind-tpcc`), profile YAML, and result packaging
remain in the main specification.

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
are to be interpreted as described in RFC 2119.

## 1. Goals

- Keep one workload model for all DBMSs: domain types, generator inputs,
  transaction workflows, terminal pacing, metrics, and integrity checks.
- Confine every SDK type (`pqxx::*`, YDB client types, OceanBase/MariaDB
  connector handles, …) inside `tpcc/dbms/<name>/`.
- Allow physical optimizations (DDL layout, query text, bulk load, fused
  commit) without forking the shared algorithm.
- Match the current PostgreSQL port where it already exists, and define the
  target abstract surface the port is migrating toward.

## 2. Layering

```text
mind-tpcc  ──SSH──>  tpcc-<dbms>  (schema | loader | indexes | worker | check)
                         │
                         ├─ shared: domain / generator / transactions /
                         │          runtime / harness / loader / checks / metrics
                         └─ adapter: tpcc/dbms/<name>/
```

| Layer | Owns | MUST NOT own |
| --- | --- | --- |
| Shared libraries | Logical schema, generator, workflows, retry policy shape, check catalog IDs, histograms; harness owns terminal loop, artifact writers, run-loop helpers, orchestrated role skeletons, clock-skew math | SQL dialects, SDK types, connection strings with secrets |
| Adapter | DDL, physical keys/partitions, query text, `PutBatch`, error mapping | Workload mix, terminal identity, phase schedule |
| Binary `tpcc-<dbms>` | CLI roles, wiring factory → runtime | A second copy of the workload model |

Shared code talks to the database only through the adapter interfaces below.
PostgreSQL maps semantic ops in `tpcc/dbms/pgsql/tpcc_session.cpp`; shared
workflows live under `tpcc/transactions/`. New adapters MUST target the same
abstract session API.

## 3. Shared Libraries

### 3.1. `tpcc/domain`

- Logical table and column names (`warehouse`, `district`, …).
- Scale constants (districts per warehouse, customers per district, item
  count, …).
- NURand / string helpers and RNG wrappers.
- Exact decimal / money types (target: checked fixed-point; **MUST NOT** use
  `double` in the domain↔adapter value path).

Money and tax fields that cross the adapter boundary MUST use exact types.
Adapters map them to `DECIMAL` / YDB `Decimal` / OceanBase `DECIMAL`, never to
binary floating point.

### 3.2. `tpcc/generator`

Produces:

- initial population rows for each logical table;
- per-transaction logical inputs (New-Order line set, Payment customer
  selection, …).

For a given `run-config` (scale, seed, …), logical contents MUST be identical
across adapters and across loader/worker counts. Parallelism MUST NOT change
row values. Inputs for a business transaction are fixed before the first
attempt and reused on retry.

### 3.3. `tpcc/transactions`

Shared business workflows over `ITpccSession` / `ITpccTransaction`. Workflows
express **semantic operations** (read customer by id, update stock quantity,
insert order line, …), not SQL strings. Semantic ops live in `ops.h`; the five
TPC-C workflows are shared under `tpcc/transactions/`.

### 3.4. `tpcc/runtime`

- Terminal state machines, keying/think times, admission / inflight limits.
- Coroutine scheduler and futures (`TFuture`, task queue,
  `TSuspendWithFuture`). After an incomplete adapter future, workflows resume
  only on the task queue; they MUST NOT be resumed on IO / SDK callback
  threads (`tpcc/runtime/coro_traits.h`).
- Phase controller (prepare → ramp-up → measurement → drain).
- Retry loop driven by normalized error classes (§5).
- Bounded drain for async Delivery-style work.

Runtime depends on domain, transactions, metrics, and the abstract adapter
API only.

### 3.5. `tpcc/loader`

Builds deterministic batches for the loader’s warehouse ranges (and the single
DB-wide shard owner), then calls `ILoadAdapter::PutBatch`. Cardinality and
sample checks after load are shared; query text is adapter-owned. The PostgreSQL
adapter currently regenerates deterministic rows from seed when row payloads
are empty.

### 3.6. `tpcc/checks`

Shared check **catalog**: identifier, expected semantics, result shape.
Adapters supply the DBMS-specific query or scan that evaluates each check.
This is integrity / infrastructure checking, not TPC-C edition conformance.
PostgreSQL, YDB, and OceanBase each evaluate the same catalog
(`TPgCheckAdapter` / `TYdbCheckAdapter` / `TObCheckAdapter`). Scheduling
(parallel sessions, `kWarehouseCheckRange`) is a shared requirement
(specification §9.2), not an adapter-private optimization.

### 3.7. `tpcc/metrics`

Mergeable counters and latency histograms. Workers emit raw histograms;
`mind-tpcc consolidate` merges buckets and only then computes percentiles.
Adapters MUST NOT emit final p99 as the authoritative result.

## 4. Adapter Interfaces

Each `tpcc/dbms/<name>` implements the following. Names are logical; C++
headers may use `I…` prefixes consistent with the repository style.

### 4.1. `IAdminAdapter`

Lifecycle of the physical database objects for one workload path:

| Method | Semantics |
| --- | --- |
| `EnsureSchema` | Create logical tables (idempotent). MAY drop/recreate when the path is empty; MUST NOT destroy foreign data silently. |
| `EnsureIndexes` | Create access paths required by the workload **after** bulk load. MUST be idempotent (skip existing indexes). |
| `EnsureStatistics` | `ANALYZE` / equivalent so the planner or tablet layer is ready. MUST be idempotent. |
| `Clean` | Remove all objects for this path. |
| `Describe` | Adapter/server version strings for `result.json`. |

Pipeline order for physical objects: `EnsureSchema` → bulk `PutBatch` (load)
→ `EnsureIndexes` → `EnsureStatistics`. Loaders MUST NOT call index/statistics
helpers; the dedicated `indexes` role owns that step.

There is no `AcquireFence` / `ReleaseFence`. Multi-host start synchronization
uses wall-clock `--start-at` (specification §7), not DBMS metadata.

PostgreSQL reference: `InitSync` + `CreateIndexes` (`init.*`), `CleanSync`
(`clean.*`), path checks (`path_checker.*`). The `indexes` role creates
secondary indexes after import; `ANALYZE` SHOULD follow.

### 4.2. `ILoadAdapter`

```text
PutBatch(table, key_range, rows) -> completed | outcome_unknown | failed
```

Normative properties (specification §6):

- Idempotent for the same `run_id` / logical batch identity: retries MUST leave
  the same final rows.
- Rows are fully determined by the run-config (scale, seed, …). No
  server-default timestamps or sequences during load.
- Exactly one loader owns DB-wide tables (`item`, and any other global
  population); others own disjoint warehouse ranges.
- `PutBatch` is semantic: the adapter MAY use `COPY`, `BulkUpsert`, staging +
  replace-range, or upsert. The loader MUST NOT depend on SQL `INSERT`.

Optional helpers: `EnsureEmptyRange`, local checkpoint of completed batches
(optimization only; absence MUST still be correct via retry).

PostgreSQL reference: `ImportSync` with `COPY` via `PgSession::ExecuteCopy`,
warehouse-range sharding, `OwnsGlobalData`.

### 4.3. Session and transaction API

Normative surface (also sketched in specification §4.2). Methods are
**asynchronous** and return `TFuture<…>` from `tpcc/runtime` (resolved open
decision: coroutine/`TFuture` ABI).

```text
ISessionFactory::CreateSession() -> ITpccSession   # MAY be sync if it does not block on the DBMS

ITpccSession::Begin(isolation) -> TFuture<ITpccTransaction>

ITpccTransaction::Execute(op)              -> TFuture<TOperationResult>
ITpccTransaction::ExecuteBatch(ops)        -> TFuture<TBatchResult>
ITpccTransaction::ExecuteFinalAndCommit(op)-> TFuture<{TOperationResult, TCommitResult}>
ITpccTransaction::Commit()                 -> TFuture<TCommitResult>
ITpccTransaction::Rollback()               -> TFuture<TCommitResult>
ITpccTransaction::Cancel()                 -> TFuture<TCommitResult>
ITpccTransaction::ExecuteSelect1()         -> TFuture<TOperationResult>  # --simulate-select1 probe
```

`CreateSession` MAY be synchronous if session construction does not block on
the DBMS. If acquire contacts the DBMS (YDB `GetSession`, …), that wait MUST
run off the task-queue thread (async `Begin`, a non-blocking factory, or a
bounded `IExecutor`) — not as `.Get()` / `GetValueSync()` on the scheduler.

#### 4.3.0. Async caller rules (worker hot path)

`ITpccSession::Begin` and every `ITpccTransaction` method that touches the
database (`Execute`, `ExecuteBatch`, `ExecuteFinalAndCommit`, `Commit`,
`Rollback`, `Cancel`, `ExecuteSelect1`) MUST be asynchronous: they MUST
return a `TFuture` that MAY still be incomplete when the method returns.

The **caller** of those methods MUST NOT block waiting for DBMS IO.
Implementations MUST NOT call `.Get()`, `GetValueSync()`, or an equivalent
blocking wait on the task-queue (scheduler) thread inside those methods.
Shared workflows await the outer future with `TSuspendWithFuture` and resume
only on the task queue (`tpcc/runtime/coro_traits.h`: do not resume
task-queue coroutines on IO / SDK callback threads).

**Session / connector layer:** adapters MAY run blocking SDK IO (libpqxx,
MariaDB C API, …) on a bounded `IExecutor` and complete a `TPromise` from
that pool (`PgSession`, `TObSession`). YDB SHOULD use the SDK async API and
bridge to `NTpcc::TFuture`. That offload is an implementation of the session
layer. It does **not** license the `ITpccTransaction` wrapper to wait on the
scheduler thread.

**Preferred adapter style:** keep `ITpccTransaction` signatures unchanged.
Inside `Execute(op)` (and the other entry points), chain session/SDK futures
with `Subscribe` (or a small shared helper), parse results, and issue the
next round-trip without blocking the caller. Continuations MAY run on the
IO / SDK callback thread; they MUST only complete promises and parse local
results — they MUST NOT touch task-queue coroutine state. The outer
incomplete future is what `TSuspendWithFuture` awaits.

**Optional later:** pass `ITaskQueue` + a thread hint into the transaction
and implement multi-step ops as coroutines with `co_await TSuspendWithFuture`
when chaining becomes hard to maintain.

Loader, indexes, check, and schema admin paths MAY still wait synchronously
until a follow-up. The rules above are the **worker** hot path.

**Current implementations** (PostgreSQL, OceanBase, YDB) still complete
`ITpccTransaction` methods with a blocking wait before returning a ready
future. That collapses `Inflight` to scheduler `ThreadCount` even when
`max_inflight` is large. The target model and migration sequence are in
[async-adapter-transactions.md](async-adapter-transactions.md). Until that
lands, paced runs SHOULD pin worker threads rather than relying on auto
`threads≈warehouses/1000` — see [parameter-reference.md](parameter-reference.md)
(`runtime.threads_per_worker`).

#### 4.3.1. Operation results and semantic encoding

`TOperationResult` carries success/failure, expected vs actual cardinality,
normalized `EErrorClass`, and native diagnostics. Cardinality mismatch is an
`integrity` error, not a successful empty read. Row/payload results use a
typed variant (or op-specific out fields) — not raw SDK row types.

`TCommitResult` carries `ECommitOutcome` (`Committed`, `RolledBack`,
`OutcomeUnknown`) plus the same error classification fields.

Transaction states: `active` → (`committing`) → `committed` |
`rolled_back` | `outcome_unknown`. After a terminal state the handle MUST NOT
accept further `Execute*` calls.

**Semantic operation encoding:** a closed set of structs in
`tpcc/transactions/ops.h` (for example `TGetCustomerById`, `TUpdateStock`,
…). `Execute` takes `const TSemanticOp&` where `TSemanticOp` is a
`std::variant` of those structs (or an equivalent tag + payload). Opaque
`void*` / size pairs MUST NOT be used. Adapters switch on the alternative and
bind SQL or SDK calls.

#### 4.3.2. Why `ExecuteBatch` and `ExecuteFinalAndCommit`

| Primitive | Purpose |
| --- | --- |
| `Execute` | One semantic op → one round trip (or equivalent). |
| `ExecuteBatch` | Set-oriented optimization: YDB YQL batches, PG multi-row prepared statements / pipelining, OceanBase multi-value DML — without changing the shared workflow’s logical steps. Adapters that do not optimize MAY emulate as a sequence of `Execute` calls. |
| `ExecuteFinalAndCommit` | Fuse the last semantic operation with commit. **Required for efficient YDB** interactive transactions; elsewhere MAY be implemented as `Execute` + `Commit`. |

Shared workflows SHOULD mark the last mutating/read op of a transaction so
adapters that benefit from fusion can use `ExecuteFinalAndCommit`. Workflows
MUST NOT assume fusion always happens: semantics after success equal
`Execute` then successful `Commit`.

#### 4.3.3. Isolation

`Begin` takes `EIsolationLevel` (`ReadCommitted`, `RepeatableRead`,
`Serializable`). The adapter maps to the nearest supported level and records
the effective choice in capabilities / result settings. PostgreSQL currently
uses repeatable-read snapshot transactions in `PgSession`.

#### 4.3.4. PostgreSQL `PgSession` implementation detail

`PgSession` exposes coroutine-friendly `ExecuteQuery` / `ExecuteModify` /
`Commit` / `Rollback` / `ExecuteCopy` over libpqxx. It remains the concrete
IO layer for:

- lazy transaction start on first statement;
- bounded executor pool for blocking IO;
- shutdown cancellation via a shared flag.

The PostgreSQL runner wires the shared `TTerminal` (`tpcc/harness`) through
`TPgSessionFactory` (`ISessionFactory`); `PgSession` and `PgConnectionPool`
stay behind that boundary. Shared workflows MUST take the abstract async
`ITpccTransaction` surface, not `PgSession&`. `TPgTpccTransaction` MUST
return those incomplete `PgSession` futures to the caller (chain / map);
it MUST NOT `.Get()` them on the task-queue thread (§4.3.0).

### 4.4. `ICheckAdapter`

Evaluates shared catalog entries against the live database:

- assignment coverage / expected cardinalities for the configured scale;
- consistency conditions (the PostgreSQL port’s 3.3.2.x suite is the
  reference set);
- post-import stricter invariants (initial YTD, null carrier on undelivered
  orders, …);
- optional sample content checks once a cross-DB canonical row encoding
  exists (open decision in the main specification).

The adapter returns a structured pass/fail with native detail. In orchestrated
mode the check role writes `{run_dir}/checks/{phase}.json` on the runtime host;
`collect` copies it to `results/<run_id>/checks/`. JSON shape, process
metadata timing, query-timeout rules, check session concurrency, and
warehouse-range chunk size are in
[specification.md](specification.md) §9.1–§9.2. Adapters MUST use
`TCheckRequest.CheckConcurrency` for parallel sessions and
`kWarehouseCheckRange` (1) for warehouse-scoped scans. Dialect differences
(OceanBase `LEFT JOIN` + `UNION ALL` instead of `FULL JOIN`) are allowed;
predicates MUST match the PostgreSQL reference set.

### 4.5. `IErrorClassifier`

Maps native errors to:

| Class | Runtime action |
| --- | --- |
| `retryable_abort` | Confirmed rollback; bounded retry with backoff + jitter |
| `not_committed` | Safe retry per adapter contract |
| `ambiguous_commit` | **MUST NOT** blind-retry; resolve or fail |
| `permanent` | Fail the operation; run policy decides abort |
| `integrity` | Fail the run |
| `cancelled` | Phase stop; not a retry |

Internal SDK retries and shared runtime retries MUST share one observable
budget. Native codes and attempt counts appear in metrics / worker results.

PostgreSQL SHOULD classify by SQLSTATE (serialization failure, deadlock,
unique violation, connection failure, …). YDB SHOULD classify by status /
issues without enabling hidden `RetryOperation` loops that bypass the budget.
OceanBase SHOULD distinguish deadlock, lock wait timeout, serialization
failure, killed transaction, disconnect, and ambiguous commit.

### 4.6. `ICapabilities`

Declarative feature flags consumed by runtime and preflight:

- supported isolation levels;
- whether `ExecuteBatch` / `ExecuteFinalAndCommit` are optimized or emulated;
- whether Delivery runs asynchronously (`async_delivery`); when false,
  Delivery executes inline in the terminal and async drain is a no-op;
- cancel support;
- max recommended inflight / sessions;
- bulk-load mechanism (`copy`, `bulk_upsert`, `multi_insert`, …);
- exact decimal type name;
- optional physical features (foreign keys, partitioning style).

Capabilities and physical options that affect the run MUST appear in the
embedded result settings (specification §5 / §8), not only in logs.

### 4.7. Configuration

Adapters parse the `database` object from `run-config.json`:

- `dbms`, `endpoint`, `database`, `path`, `user`, `password_env`,
  `password_file`, `options`;
- YDB additionally: `auth_scheme` (`anonymous` | `login` | `sa_key`),
  `sa_key_file`, `ca_file`;
- passwords and tokens **never** in argv or stored secret values;
  orchestrated runs prefer `password_file` (worker-local path); standalone
  may use an environment variable named in `password_env` (and similar
  option keys);
- file secrets (`password_file`, `sa_key_file`, `ca_file`) are delivered by
  the orchestrator; run-config carries worker-local paths only (for example
  `db-password`, `sa-key.json`, `ca.pem` next to `run-config.json`).

`options` is adapter-specific (e.g. YDB `tx_mode`, PostgreSQL schema/`search_path`
policy, OceanBase FK enablement). Unknown options MUST be rejected at
validate/preflight time.

## 5. Logical vs physical schema

Shared code defines logical tables, columns, and required access patterns.
Adapters emit DDL.

Adapters MAY:

- reorder primary-key columns for locality (warehouse-leading keys on YDB);
- add partitions, tablegroups, or store options;
- add technical keys where the logical key is weak (e.g. `history`);
- add indexes beyond the minimum if recorded in settings;
- omit foreign keys when the DBMS makes them expensive, if that choice is
  recorded (OceanBase optional FKs).

Adapters MUST:

- preserve logical column semantics and transactional visibility;
- map exact domain decimals to exact SQL/YQL types;
- keep technical columns invisible to shared checks or clearly filterable;
- avoid duplicate equivalent indexes.

### 5.1. PostgreSQL (reference implementation)

- Tables in an explicit schema/`path`; prefer qualified names over relying on
  `search_path`.
- `DECIMAL` for money/tax; prepared `exec_params`; `COPY` for load.
- Secondary indexes (`idx_customer_name`, `idx_order`) after bulk load, then
  `ANALYZE`.
- Blocking libpqxx: IO MUST run on a bounded pool (current `IExecutor`
  pattern) **in `PgSession`**. `TPgTpccTransaction` MUST chain those
  incomplete futures (§4.3.0); it MUST NOT `.Get()` on the scheduler thread.
- Optional warehouse `HASH` partitioning on local tables
  (`database.options.partitioning=warehouse_hash`,
  `database.options.partition_count`); see
  [pgsql-partitioning-design.md](pgsql-partitioning-design.md).

### 5.2. YDB

- Warehouse-leading keys and range partitions for warehouse-local tables;
  document split policy in `options` / settings.
- Typed `BulkUpsert` (or equivalent) for `PutBatch`.
- Prefer set-oriented YQL and **`ExecuteFinalAndCommit`** so the last
  statement and commit are one round trip. Fusion MUST be an async
  pipeline (one incomplete outer future), not a sync wrapper around
  `GetValueSync()`.
- Bridge YDB SDK execute/commit futures to `NTpcc::TFuture` without
  `GetValueSync()` on the task-queue thread (§4.3.0). Session acquire
  (`GetSession`) MUST also leave the scheduler (async `Begin` / factory
  or executor offload).
- Do not store exact values as `Double`.
- Do not hide retries inside SDK helpers; classify and bubble errors.
- System tables, compaction, and index implementation details stay inside the
  adapter.

### 5.3. OceanBase

- Partition / tablegroup by warehouse for local tables; separate placement for
  DB-wide `item`.
- Cached prepared statements with bound parameters.
- Blocking MariaDB C API: IO MUST run on a bounded `IExecutor` **in
  `TObSession`**. `TObTpccTransaction` MUST chain those incomplete
  futures (§4.3.0); it MUST NOT `.Get()` on the scheduler thread.
- Optional foreign keys as a recorded physical option.
- Parallel `CREATE INDEX` via `PARALLEL n` (`database.options.index_parallel`,
  default 4; DOP for one index, not concurrent DDL).
- Post-index `DBMS_STATS.GATHER_TABLE_STATS` with `degree` equal to the HASH
  partition count (`1` when partitioning is off). Non-OceanBase MySQL targets
  keep `ANALYZE TABLE`.
- MariaDB-compatible connectors are fine for transport; validation MUST run
  against real OceanBase, not only MariaDB.

## 6. Query text and semantic operations

Shared workflows name **semantic operations**. Adapters bind each operation to
DBMS-specific query text (or SDK calls).

Examples:

| Semantic op | PostgreSQL sketch | YDB sketch |
| --- | --- | --- |
| Get customer by id | `SELECT … FROM schema.customer WHERE c_w_id=$1 AND c_d_id=$2 AND c_id=$3` | YQL `SELECT … WHERE …` with typed params |
| Update stock | single-row `UPDATE … RETURNING` or separate read/write | upsert / YQL update; MAY batch line items via `ExecuteBatch` |
| Insert order lines | per-line `INSERT` or multi-row `INSERT` | `BulkUpsert` / multi-row upsert inside one tx |
| Final New-Order step | last `INSERT` then `COMMIT` | `ExecuteFinalAndCommit(last_op)` |

Rules:

1. Changing query text MUST NOT change observable logical results for the same
   seed and inputs.
2. Locking hints (`FOR UPDATE`, …) are adapter-local but MUST provide the
   isolation the workflow expects.
3. `RETURNING` / multiple result sets are optional optimizations; shared code
   consumes typed operation results, not raw cursors.
4. Identifier quoting and schema qualification are adapter-local.
5. Semantic ops are the `TSemanticOp` variant described in §4.3.1; adapters
   MUST NOT invent a parallel opaque encoding.

## 7. Worker and loader binary roles

Each `tpcc-<dbms>` binary **MUST** expose:

| Role | Adapter use |
| --- | --- |
| `schema` | `EnsureSchema` (+ optional early indexes if required by the DBMS) |
| `loader` | `PutBatch` over assigned ranges only (no indexes/statistics) |
| `indexes` | `EnsureIndexes` then `EnsureStatistics` (idempotent; after all loaders finish) |
| `worker` | sessions for terminals; honor `--start-at` (specification §7); write diagnostics / `result.json` |
| `check` | `ICheckAdapter` for `--after-import` / `--after-test` (process contract: specification §9.1–§9.2) |

Orchestrated remotes pass at least `--run-config`, `--instance`, and for
workers `--start-at=<RFC3339-UTC>`.

Binaries **MAY** keep standalone aliases for local use:

| Alias | Meaning |
| --- | --- |
| `init` | ≡ `schema` (local flags; may drop/recreate) |
| `import` | standalone load without run-config assignment |
| `run` | standalone worker without run-config / `--start-at` |
| `clean` | admin helper; standalone `--connection` or mind cleanup via `--run-config --instance` |

Non-DBMS logic (assignment interpretation, phase timing, artifact layout)
comes from shared libraries and the distributed `run-config.json`.

## 8. Implementation status (informative)

| Piece | Status |
| --- | --- |
| `tpcc/domain`, `runtime`, `metrics` | Present |
| `tpcc/transactions` | Async `ITpccSession` + `TSemanticOp`; five shared workflows |
| `tpcc/generator`, `loader`, `checks` | Present; PG PutBatch still regenerates rows from seed |
| `tpcc/dbms/pgsql` | Concrete admin/load/session/check + terminal runtime; worker `ITpccTransaction` still `.Get()`s on the scheduler (gap: [async-adapter-transactions.md](async-adapter-transactions.md)) |
| `tpcc/dbms/ydb` | In progress; worker path uses `GetValueSync()` on the scheduler (same gap) |
| `tpcc/dbms/oceanbase` | Connector/C admin/load/session/check + terminal runtime; worker `ITpccTransaction` still `.Get()`s on the scheduler (same gap) |
| `mind-tpcc` | Phase 5 remote drive / consolidate present |

Alignment sequencing and accepted API decisions:
[alignment-plan.md](alignment-plan.md). Module status detail:
[dependencies.md](dependencies.md).

Adapter authors for YDB/OceanBase SHOULD implement against §4; the PostgreSQL
port is the reference adapter.

## 9. Related documents

- [specification.md](specification.md) — product architecture, config, phases,
  results, orchestrator commands, remote process contract (§9.1–§9.2);
  session surface and worker inflight vs threads (§4.2, §7).
- [async-adapter-transactions.md](async-adapter-transactions.md) — worker
  `ITpccTransaction` async migration (target model, sequence, acceptance).
- [alignment-plan.md](alignment-plan.md) — phased implementation plan and
  accepted API decisions.
- [dependencies.md](dependencies.md) — third-party libraries and port status.
- [tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md) —
  engineering vs official TPC-C 5.11 notes.
- [examples/run-config.v1.json](examples/run-config.v1.json) — concrete settings
  distributed to loaders and workers.
