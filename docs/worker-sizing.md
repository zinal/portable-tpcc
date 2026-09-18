# Worker process sizing (threads and max inflight)

How to set **one** `tpcc-<dbms>` worker process at high warehouse scale
(`scale.warehouses` ≥ **50000**), when the client is scaled by **adding
worker processes** rather than by giving one process the whole scale.

Profile knobs:

| Profile | Standalone | Meaning in this project |
| --- | --- | --- |
| `runtime.threads_per_worker` | `--threads` | Coroutine **scheduler** threads (`ComputeRunLayout`) |
| `runtime.max_inflight_per_worker` | `--max-inflight` / `-m` | Admission cap: concurrent DBMS transactions **and** (PostgreSQL / OceanBase) connection-pool size |

`0` / omit for threads keeps auto sizing on each worker. `max_inflight_per_worker`
`≤ 0` materializes **100** (same as standalone and tpcc-postgres-cpp).
Adapter `MaxRecommendedInflight` is **256** on every current DBMS.

This is **not** loader `--threads` / `threads_per_loader` and **not**
integrity-check `--threads` / `runtime.check_concurrency`.

## 1. What one worker actually does

Each worker owns a contiguous warehouse range. It creates
`warehouses × terminals_per_warehouse` terminals (default **10** per
warehouse, TPC-C 5.11). Terminals spend most of the wall clock in keying
and think time. Only terminals that have obtained an inflight slot talk to
the DBMS.

```text
terminals  (10 × assigned warehouses, mostly sleeping)
    │
    ▼  admission
max_inflight  ── PostgreSQL / OceanBase: that many TCP connections + IO threads
              ── YDB: that many concurrent Query Service sessions
    │
    ▼  scheduler
threads_per_worker  (coroutines; must not block on DBMS IO)
```

Worker `ITpccTransaction` is async on PostgreSQL, OceanBase, and YDB
([async-adapter-transactions.md](async-adapter-transactions.md)). Many
transactions MAY be in flight per scheduler thread, up to `max_inflight`.
If `Inflight` stays glued to `ThreadCount` while `max_inflight` is larger,
the scheduler is blocked on IO — that is a bug, not a reason to raise
`--threads`.

Auto threads (`threads_per_worker: 0`):

```text
recommended = ceil(assigned_warehouses / 1000)   # WAREHOUSES_PER_CPU_CORE
threadCount = min(recommended, terminals, CPU − reserved_for_IO)
# then bump to even when a spare core remains
```

(`tpcc/harness/run_loop.cpp` `ComputeRunLayout`; same heuristic as
`ydb workload tpcc` / tpcc-postgres-cpp.)

Pool / IO sizing when `--io-threads` is left at default **0**:

```text
PoolSize  = min(terminals, max_inflight)
IoThreads = max(max_inflight, PoolSize)   # → equal to max_inflight
```

PostgreSQL and OceanBase **pre-create** `PoolSize` connections and an
`IExecutor` of `IoThreads` OS threads at process start. YDB does **not**
use that IO pool: sessions are taken lazily from QueryClient
(`MaxActiveSessions = 8192`, `MinPoolSize = 0`).

## 2. Why scale by adding workers

50 000 warehouses ⇒ **500 000** terminals if packed into one process.
That is a bad client layout even though think time keeps most terminals
idle:

- Each worker sleeps **1 ms per terminal** while starting them, then must
  finish prepare before `--start-at` (specification §7). 5 000 warehouses
  on one process is already ~50 s of that sleep plus pool setup.
- PostgreSQL / OceanBase would try to open `max_inflight` connections
  **and** `max_inflight` IO threads **in that one process**.
- YDB developers recommend **at most ~5 000 warehouses per client
  instance**, and about **1 client core per 1 000 warehouses**
  ([ydb-platform/tpcc](https://github.com/ydb-platform/tpcc) hardware
  notes). The in-tree `WAREHOUSES_PER_CPU_CORE = 1000` matches that.

**Target shard for one worker at this scale: 1 000–2 000 warehouses**
(hard ceiling **5 000**). For 50 000 warehouses that is **25–50 worker
processes**, listed under `workers:` (repeat a host string to co-locate).

Do **not** raise `max_inflight` so that one process can “cover” 50 000
warehouses. Admission is per process; extra warehouses only add sleeping
terminals and prepare time. Throughput at paced TPC-C is increased by
more warehouses **spread across workers**, not by a larger pool in one
binary.

## 3. How large `max_inflight` needs to be (paced runs)

With default keying/think times the mix spends ~**21 s** off the DBMS per
transaction. Little’s law for one worker:

```text
tx/s     ≈ assigned_warehouses × 10 / 21
inflight ≈ (tx/s) × mean_DBMS_latency
```

| Warehouses / worker | tx/s (paced) | Inflight @ 50 ms | @ 100 ms | @ 200 ms | @ 500 ms |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 000 | ~476 | ~24 | ~48 | ~95 | ~238 |
| 2 000 | ~952 | ~48 | ~95 | ~190 | ~476 |
| 5 000 | ~2 381 | ~119 | ~238 | ~476 | ~1 190 |

So for a **healthy** paced worker (New-Order well inside the 5 s constraint):

- **100** (project default) covers ~200 ms mean latency at 1 000 WH, or
  ~100 ms at 2 000 WH.
- **256** (`MaxRecommendedInflight`) is the usual ceiling per process:
  headroom for 1 000–2 000 WH when the DBMS is slower, without opening
  thousands of sessions.

If `Inflight` sits on the cap and `ready` is large while the DBMS still
has CPU headroom, raise `max_inflight_per_worker` (still prefer ≤ 256)
**or** split the shard. If the DBMS is already saturated, more inflight
only adds contention.

`--no-delays` / `runtime.pacing: disabled` is a different workload: almost
every ready terminal wants a session. Do not use the numbers below for
that mode.

## 4. Per-DBMS recommendations (one worker)

Assumptions: paced TPC-C 5.11-style defaults; worker shard **1 000–2 000**
warehouses; many workers; client hosts separate from the DBMS.

### 4.1. YDB (`tpcc-ydb`)

Developer guidance (YDB CLI TPC-C, same lineage as this adapter):

- About **50 warehouses per compute (dynnode) CPU core** as a cluster
  scale starting point.
- Sessions: start at **5 × dynnode cores**, increase toward **10×** if
  the cluster is underutilized
  ([ydb workload tpcc](https://ydb.tech/docs/en/reference/ydb-cli/workload-tpcc)).
  The CLI auto path uses **15 sessions per compute core** “according to
  our runs” (`SESSIONS_PER_COMPUTE_CORE` in `ydb/library/workload/tpcc`).
- A YDB session is a **server-side actor**. Size the **cluster-wide**
  session count, not “as many as the SDK will hold”
  ([session pool limit](https://ydb.tech/docs/en/recipes/ydb-sdk/session-pool-limit)).
  Native SDK default is 50; this adapter raises QueryClient
  `MaxActiveSessions` to **8192** so a large worker is not clipped at 50
  — that is a ceiling, not a target.
- Client query threads: keep to roughly **50–75% of the client host
  CPUs** so the SDK network threads are not starved. Auto
  `ceil(WH/1000)` already stays in that band when the shard is 1 000–2 000 WH.

This adapter: one `TDriver` + QueryClient per process, `UseAllNodes`,
`session-balancer` on `CreateSession`. Isolation default `snapshot-rw`.
No extra IO thread pool; `layout.IoThreads` is unused by the YDB runner.

| Knob | Recommendation per worker | Notes |
| --- | --- | --- |
| `threads_per_worker` | **0** (auto) or pin **2–4** | Auto is ~2 at 1 000 WH, ~2–4 at 2 000 WH. Pin when co-locating several workers on one host (each process otherwise assumes it owns every CPU). |
| `max_inflight_per_worker` | **100** to start; **256** if dynnodes are idle and `Inflight` is at the cap | Equals concurrent Query sessions from this process. |

Cluster-wide check:

```text
workers × max_inflight_per_worker  ≲  5…15 × (YDB compute cores)
```

Example: 50 000 WH on a cluster sized at ~50 WH/core ⇒ ~1 000 compute
cores. Session budget **5 000–15 000**. Fifty workers × **100** = 5 000
sessions (5×); fifty × **256** = 12 800 (near 15×). Do not run 50 × 256
against a small database.

### 4.2. PostgreSQL (`tpcc-pgsql`)

This worker **is** the connection pool: `max_inflight` backends, not one
libpq connection per terminal. That matches the PostgreSQL wiki
([Number Of Database Connections](https://wiki.postgresql.org/wiki/Number_Of_Database_Connections)):
queue at the client once the server’s resources are in use. A durable
starting formula for **active** connections is about
`(physical_cores × 2) + effective_spindles` (spindles → 0 when the
working set is cached). Past that “knee”, more backends typically lose
throughput to RAM (`work_mem` × backends), locks, and context switches.

Each worker also starts **`max_inflight` IO threads** (libpqxx is
offloaded to `IExecutor`). Co-locating many workers with `max_inflight`
256 creates thousands of client threads; pin threads and keep the pool
modest on shared hosts.

At 50 000 warehouses use `database.options.partitioning: warehouse_hash`
([pgsql-partitioning-design.md](pgsql-partitioning-design.md)); modulus
max **1024**.

| Knob | Recommendation per worker | Notes |
| --- | --- | --- |
| `threads_per_worker` | **0** (auto) or pin **2–4** | Same WH/1000 heuristic. Pin when co-locating. |
| `max_inflight_per_worker` | **64–128** typical; **256** only if `max_connections` and wiki budget allow | Pre-opened connections. |

Cluster-wide check:

```text
workers × max_inflight_per_worker  +  admin slots  <  max_connections
workers × max_inflight_per_worker  ≲  ~2× PostgreSQL physical cores   # first try
```

Worked example: 50 workers × 100 connections = **5 000** backends — too
many for a single PostgreSQL. Prefer **fewer connections per worker**
and/or **fewer workers with larger shards** so the **sum** stays near a
few hundred (up to ~1 000 on a very large, well-cached machine).

Little’s law still applies to the **sum**: 50 000 WH at 50 ms mean
latency wants ~1 200 concurrent transactions globally. If that already
exceeds a healthy `max_connections`, extra inflight will not help;
latency and efficiency will show it. A fast in-memory PG (~10 ms) needs
only ~240 global connections — 25 workers × **16** or 50 × **8** can be
enough. Raise per-worker inflight only while `Inflight` is at the cap
**and** the server is not in the wiki “knee”.

### 4.3. OceanBase (`tpcc-oceanbase`)

`obd test tpcc` defaults (`--terminals=800` at 1 000 warehouses, range
`(0, 10 × warehouses]`) size **BenchmarkSQL-style concurrent threads**,
not this client’s 10 sleeping terminals + pool. Do not copy `800` into
`max_inflight_per_worker`.

This adapter: one TCP connection per inflight slot; each session caches
on the order of **30** `COM_STMT_PREPARE` handles once warm (`EObQueryId`,
documented in [run-oceanbase.md](run-oceanbase.md)). Observer **-4013**
(tenant memory) on PREPARE is the usual failure when the pool is too
large. Tenant session caps also exist (`max_connections`,
`_resource_limit_max_session_num`; a small tenant is often
`max(100, tenant_memory × 5% / 100 KB)`).

Measured in this repo after the async conversion: **2 threads**,
`max_inflight=100`, `Inflight` in **30–100** at ~1 200 WH/worker
(`docs/async-adapter-transactions.md`, `docs/run-oceanbase.md`).

| Knob | Recommendation per worker | Notes |
| --- | --- | --- |
| `threads_per_worker` | **0** (auto) or pin **2–4** | Auto ≈ 2 at 1 000–1 200 WH. |
| `max_inflight_per_worker` | **100** to start; **128–256** if observers are idle and there is no -4013 | Each connection costs tenant memory × ~30 prepared statements. |

Cluster-wide check:

```text
workers × max_inflight_per_worker  <  tenant session limit
MEMORY_SIZE  sized for  (total connections × ~30 statements), not only memstore
```

If **-4013** appears, **lower** `max_inflight_per_worker` (or grow
`MEMORY_SIZE`). Adding more workers at the same inflight **increases**
total prepared-statement memory.

HASH partitions: at schema time `database.options.partitions` **0**
derives `N = warehouses` (max 8192). For 50 000 WH either cap `N` (for
example 1024–8192) or accept 50 000 partitions — that is a schema choice,
independent of per-worker inflight.

## 5. Suggested profile fragments (≥ 50 000 WH)

Keep `threads_per_worker: 0` unless several workers share a host; then
pin **2** (or **4** on ≥16-core client boxes) so auto does not
oversubscribe.

```yaml
scale:
  warehouses: 50000

# 50 processes × 1000 WH, or 25 × 2000. Repeat a host to co-locate.
workers:
  - 10.10.0.31
  - 10.10.0.32
  # … 25–50 entries …

runtime:
  pacing: enabled
  think_time_distribution: exponential
  threads_per_worker: 0          # auto: ceil(WH_per_worker / 1000)
  max_inflight_per_worker: 100   # see table below
```

| DBMS | `threads_per_worker` | `max_inflight_per_worker` | First thing to watch |
| --- | --- | --- | --- |
| YDB | `0` (or `2`–`4` if co-located) | `100`, try `256` if dynnodes idle | `workers × inflight` vs 5–15 × compute cores; session actors |
| PostgreSQL | `0` (or `2`–`4` if co-located) | `64`–`128` (rarely `256`) | `workers × inflight` vs `max_connections` and ~2× server cores |
| OceanBase | `0` (or `2`–`4` if co-located) | `100`, try `128`–`256` if no -4013 | tenant `MEMORY_SIZE` and session limit |

`phases.start_lead` must cover terminal start (≈ 1 ms × terminals) plus
PostgreSQL/OceanBase pool connect. For 2 000 WH/worker budget **well
above 45 s** (on the order of a minute or more).

## 6. How to confirm a worker is sized well

Progress lines already print `Inflight`, `ThreadCount`, and scheduler
`ready` (every `runtime.stats_interval`, default **30 s**). Healthy paced
worker:

1. **`Inflight` > `ThreadCount`** when the DBMS has headroom (often tens
   to low hundreds). If it stays equal to `ThreadCount` for three consecutive
   progress samples with a deep `ready` queue, the worker logs the
   inflight-stuck warning — that is scheduler blocking, not “need more
   threads”.
2. **`Inflight` not glued to `max_inflight`** for the whole measurement.
   Persistent cap + high `ready` + idle DBMS ⇒ raise inflight or split
   the shard. Persistent cap + busy DBMS ⇒ stop raising inflight.
3. **Efficiency** near the TPC-C ceiling (12.86 tpmC/warehouse) once
   latency constraints are met. Low efficiency with small inflight and
   idle servers ⇒ more sessions or faster SQL. Low efficiency with huge
   inflight ⇒ contention; follow the vendor caps above.
4. PostgreSQL: backends ≈ `workers × max_inflight`. OceanBase: no -4013
   on `COM_STMT_PREPARE`. YDB: session count on dynnodes stays in the
   5–15× cores band.

## 7. Related

- [parameter-reference.md](parameter-reference.md) — field definitions
- [specification.md](specification.md) §7 — admission vs scheduler threads
- [async-adapter-transactions.md](async-adapter-transactions.md) — why
  auto `threads ≈ WH/1000` is valid
- [run-ydb.md](run-ydb.md), [run-pgsql.md](run-pgsql.md),
  [run-oceanbase.md](run-oceanbase.md)
