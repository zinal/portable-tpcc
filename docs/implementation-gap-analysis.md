# Implementation vs Design Documents — Gap Analysis

Status: analysis of repository HEAD at the time of writing.
Scope: design docs in `docs/` versus code under `tpcc/` and `tools/tpccctl/`.
Not in scope: official TPC certification; this project explicitly does not claim it
([specification.md](specification.md) §1).

Related documents:

- [specification.md](specification.md)
- [adapter-api.md](adapter-api.md)
- [dependencies.md](dependencies.md)

## 1. Executive summary

The repository matches the **documented transitional state**: a substantial
PostgreSQL TPC-C port plus early shared infrastructure and a scaffold
orchestrator. It does **not** yet meet the normative architecture end-to-end.

| Area | Design target | Current state |
| --- | --- | --- |
| Shared domain / exact decimals | MUST exact types, no `double` in money path | Constants + RNG helpers; money uses `double`; `c_ytd_payment` is SQL `float` |
| Shared generator / loader / checks | Separate packages | **Absent**; logic lives inside `tpcc/dbms/pgsql` |
| Abstract adapter API (`IAdmin` / `ILoad` / session / …) | Normative for adapters | Only incomplete `session.h` skeleton; PG does not implement it |
| PostgreSQL concrete TPC-C | Reference port | Schema, COPY load, 5 txns, terminals, checks — usable standalone |
| Orchestrated multi-host run | `tpccctl` + start-token + merge | Config/assignment mostly done; remote drive and sync largely scaffold |
| YDB / OceanBase | Planned adapters | **Not started** |

**Bottom line:** PostgreSQL can run a recognizable TPC-C-style workload locally
(`init` / `import` / `run` / `check`). Horizontal, deterministic, mergeable,
specification-conformant runs are not complete.

---

## 2. Repository layout (§12)

| Spec path | Present? |
| --- | --- |
| `tpcc/domain` | Yes (partial) |
| `tpcc/generator` | **No** |
| `tpcc/transactions` | Skeleton only (`session.h`) |
| `tpcc/runtime` | Yes (coroutines / futures / queues; no shared terminals/phases) |
| `tpcc/loader` | **No** |
| `tpcc/checks` | **No** |
| `tpcc/metrics` | Yes (`THistogram`) |
| `tpcc/dbms/pgsql` | Yes (concrete) |
| `tpcc/dbms/ydb`, `oceanbase` | **No** |
| `tpcc/app/pgsql` | Yes (`tpcc-pgsql`) |
| `tpcc/app/ydb`, `oceanbase` | **No** |
| `tools/tpccctl` | Yes (early) |
| `docs/` + examples | Yes |

This matches the informative status table in [adapter-api.md](adapter-api.md) §8
and [dependencies.md](dependencies.md).

---

## 3. Shared libraries vs adapter-api.md

### 3.1. `tpcc/domain`

**Present:** scale constants, table names, mix/keying/think defaults, NURand
helpers, process utilities (`constants.h`, `rng.h`, `domain_util.*`).

**Gaps (MUST):**

- No exact decimal / money type; `DISTRICT_INITIAL_YTD` is `double`.
- No logical row/column types or strong identifiers.
- RNG is thread-local and seeded from `std::random_device`, not from
  `run-config` `data.seed`.
- Seed is parsed in the PG run-config parser and never consumed for generation.

### 3.2. `tpcc/runtime`

**Present:** `TFuture` / task queue / timer queue / thread pool / logging —
used by the PostgreSQL adapter.

**Gaps:** terminal state machine, phase controller (prepare → ramp-up →
measurement → drain), normalized retry loop, backoff/jitter, async Delivery
drain, and start-token admission all live in `tpcc/dbms/pgsql` (or are missing),
not in shared runtime.

### 3.3. `tpcc/metrics`

**Present:** in-memory `THistogram` with `Add` / percentile computation.

**Gaps (MUST):**

- No serialization of raw buckets for worker `result.json`.
- Workers emit precomputed p50/p90/p99 (`artifacts.cpp`), contrary to
  specification §8 (“Workers do not compute final percentiles”).
- Run-config histogram settings are ignored.

### 3.4. `tpcc/transactions`

**Present:** enums (`EIsolationLevel`, `EErrorClass`, `ECommitOutcome`),
`TOperationResult` / `TCommitResult`, `ITpccSession` / `ITpccTransaction` /
`ISessionFactory`.

**Gaps:**

- No `ExecuteBatch` / `ExecuteFinalAndCommit` (required by adapter-api §4.3).
- Opaque `ExecuteSemantic(const void*, size_t)` instead of typed semantic ops.
- No shared business workflows; PG SQL remains in `transaction_*.cpp`.
- `PgSession` does not implement these interfaces.
- Header uses `std::unique_ptr` without including `<memory>`.

### 3.5. Missing shared packages

`tpcc/generator`, `tpcc/loader`, `tpcc/checks` do not exist. Population,
`PutBatch`-equivalent COPY, and integrity suites are PostgreSQL-local.

### 3.6. Missing adapter interfaces

No definitions or implementations for:

- `IAdminAdapter` (incl. `AcquireFence` / `ReleaseFence` / `Describe`)
- `ILoadAdapter` / `PutBatch`
- `ICheckAdapter`
- `IErrorClassifier`
- `ICapabilities`

Fence-related symbols are absent from the tree.

---

## 4. PostgreSQL implementation completeness

### 4.1. What works (standalone port)

| Capability | Location | Notes |
| --- | --- | --- |
| 9-table schema + FKs + PKs | `init.cpp` | Destructive drop-then-create |
| Secondary index `idx_customer_name` | `import.cpp` (`CreateIndexes` from `init.cpp`) | Created after bulk load, before ANALYZE |
| COPY bulk load (all tables) | `import.cpp` | Warehouse-range + `OwnsGlobalData` for `item` |
| New-Order | `transaction_neworder.cpp` | 5–15 lines, 1% remote, 1% invalid item + rollback |
| Payment | `transaction_payment.cpp` | 85/15, 60/40 name/id, median-by-first-name |
| Order-Status | `transaction_orderstatus.cpp` | Latest order + lines |
| Delivery | `transaction_delivery.cpp` | Per-district oldest new-order mutations |
| Stock-Level | `transaction_stocklevel.cpp` | Last 20 orders, low-stock distinct items |
| Terminals + mix pacing | `terminal.cpp` | 10 terminals/WH; hardcoded mix/timings |
| Connection / executor pools | `pg_session.*`, `pg_connection_pool.*` | Repeatable-read; bounded blocking IO |
| Cardinality + 3.3.2.x checks | `check.cpp` | Strongest conformance-oriented piece |
| Legacy CLI | `app/pgsql/main.cpp` | `init`, `import`, `run`, `clean`, `check` |
| Orchestrated roles | `worker_loader.cpp` | `worker` / `loader` + run-config assignment |

### 4.2. Critical gaps vs specification / adapter-api

1. **Non-deterministic, non-idempotent load**  
   Seed ignored; wall-clock timestamps in rows; plain `COPY` (retries → PK
   conflicts); no `PutBatch` batch identity. Multi-loader preflight races on
   “warehouse empty”.

2. **No start-token synchronization**  
   Workers write `ready.json` then start immediately on local clocks. Absolute
   phase timestamps from `start-token.json` are not consumed.

3. **Incorrect measurement / drain semantics**  
   Warmup clear is racy across terminals. “Drain” continues admitting new
   transactions; metrics and measurement duration include drain work. Async
   Delivery queue / `async_work_drain` absent (Delivery is synchronous).

4. **Workers emit percentiles, not mergeable histograms**  
   Contradicts §8; `tpccctl consolidate` cannot recompute authoritative p99.

5. **No SQLSTATE / normalized error classes**  
   Only `pqxx::transaction_rollback` retries (hardcoded 3). No
   `ambiguous_commit` handling; `retry.max_attempts` parsed but unused.

6. **Exact decimals violated**  
   Domain/adapter path uses `double`; `customer.c_ytd_payment` is `float` in
   DDL (`init.cpp`).

7. **Run-config partially ignored**  
   `workload` (mix, keying/think, terminals/WH), histogram, most retry fields,
   `start_lead_ms` / `stop_grace_ms`, `database.options` not applied.

8. **CLI roles transitional**  
   Target: `schema | loader | worker | check`. Actual: legacy
   `init`/`import`/`run`/`clean`/`check` plus `loader`/`worker`. No run-config
   `check` role; no `schema` alias.

9. **No DB-scoped fence**  
   Spec §7; neither adapter nor orchestrator implements it (orchestrator uses
   a local profile lock instead).

10. **Secrets policy incomplete**  
    Orchestrated `password_env` works in the normal conninfo builder, but
    endpoint strings containing `=` are passed through unchanged (bypass risk),
    and legacy `--connection` may put secrets in argv.

### 4.3. TPC-C logical fidelity (engineering, not certification)

Recognizable and largely complete for engineering use:

- Scale factors (10 districts, 3000 customers, 100k items, …).
- Five transaction types with core SQL access patterns.
- Mix weights and keying/think defaults match common TPC-C defaults
  (hardcoded; not profile-driven).
- Fixed transaction inputs across rollback retries
  (`FixedTransactionInputs`) — timestamps still regenerate via
  `CURRENT_TIMESTAMP`.
- Integrity suite covers most 3.3.2.x conditions plus post-import invariants.

Notable deviations / weaknesses:

| Topic | Issue |
| --- | --- |
| Money | `double` / `float` instead of exact decimal throughout |
| Load strings | a-strings are letters-only; “ORIGINAL” via probability, not exact 10% |
| Delivery | Synchronous; no deferred/queued execution; no `FOR UPDATE` / `SKIP LOCKED` on oldest new-order |
| Payment (1 WH) | “Remote” branch may pick another district in the same warehouse |
| Cardinality | Many queries do not treat row-count mismatches as `integrity` errors |
| Checks | Some 3.3.2 queries miss empty groups / mixed delivery-date cases (inner joins / NULL semantics) |
| Isolation | Fixed repeatable read; not selected via `Begin(isolation)` or recorded in capabilities |

**Verdict on TPC-C completeness for PostgreSQL:** the **database workload
core** (schema, load shape, five transactions, basic integrity checks) is
largely present as an engineering port. The **product completeness** required
by the design docs (deterministic multi-host load, synchronized measurement,
mergeable metrics, abstract adapter surface, orchestrated lifecycle) is
substantially incomplete.

---

## 5. Orchestrator `tpccctl` vs specification §5 / §8 / §9

| Command | Status |
| --- | --- |
| `validate` | Partial (profile checks; not fully strict) |
| `plan` | Mostly done (defaults, balanced-contiguous assignment, run-config) |
| `deploy` | Local prototype (no SSH multi-host) |
| `schema` / `load` / `check` / `start` / `stop` | Scaffold (state transitions; do not drive binaries) |
| `status` | Reads local state; `latest` not resolved |
| `collect` | Helper exists; CLI path mostly scaffold |
| `consolidate` | Sums counters only; hardcoded status flags; no histogram merge |
| `run` | Incomplete pipeline; omits after-run check recording as designed |
| `cleanup --yes` | Local cleanup implemented |

Other gaps: no `start-token.json` writer/consumer; no process supervision via
`process.json`; no DB fence; YDB/OceanBase accepted by validation without
binaries.

Examples under `docs/examples/` describe the target artifacts
(`profile`, `run-config`, `start-token`, `aggregate`); several fields are
documentation-ahead of runtime.

---

## 6. Alignment with “Done When” (specification §13)

| Criterion | Met? |
| --- | --- |
| Same seed → equivalent logical data on all adapters | **No** (seed unused; only one adapter) |
| Multi-host workers without duplicate home terminals | Assignment planned in `tpccctl`; sync/launch incomplete |
| Phases sync; warmup excluded from measurement | **No** (local clocks; racy warmup clear) |
| Load safely retryable; post-import checks pass | Checks exist; load **not** retry-safe |
| Aggregate embeds settings; merge from raw without DBMS | Partial / incorrect merge semantics |
| No secrets in stored configs/results | Mostly for orchestrated path; bypasses remain |

Open decisions from specification §14 (coroutine ABI vs session API,
histogram layout, ambiguous-commit policy, canonical row bytes, min DBMS
versions) remain unresolved in code.

---

## 7. Priority gaps (recommended order)

Correctness / architecture blockers first:

1. Deterministic seeded generator + idempotent `PutBatch` load path.
2. Exact decimal types on the domain↔adapter money path (fix `c_ytd_payment`).
3. Start-token absolute phases; stop admission before drain; fix measurement windows.
4. Raw histogram buckets in worker results; true bucket merge in consolidate.
5. SQLSTATE → `EErrorClass` classifier; honor retry config; ambiguous-commit policy.
6. Lift workflows onto `ITpccSession` (`Execute` / batch / final-and-commit).
7. Shared check catalog + structured check artifacts; run-config `check` role.
8. DB fence; SSH deploy / process supervision in `tpccctl`.
9. Full run-config consumption (workload, histogram, options validation).
10. YDB / OceanBase adapters (blocked on SDK packaging per dependencies.md).

---

## 8. What the design docs already admit

This analysis is consistent with the docs’ own informative status:

> PostgreSQL binary still embeds some SQL inside `transaction_*.cpp`; that is
> transitional. … Shared generator / loader / checks packages: Not started.
> — adapter-api.md §2 / §8

> `tpcc/dbms/pgsql` — done (initial port); `tools/tpccctl` — in progress;
> unit/integration tests — not yet ported.
> — dependencies.md

The gap is therefore primarily **implementation lag behind a clear target**,
not undocumented scope creep. Closing the gaps above is what turns the current
PostgreSQL port into the reference adapter the architecture describes.
