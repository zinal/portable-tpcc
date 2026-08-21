# Async `ITpccTransaction` migration plan

Status: **in progress** (PR A / PostgreSQL worker path).  
Related contract: [adapter-api.md](adapter-api.md) §4.3 (transaction ops that
touch the DBMS MUST be async), §5 (PostgreSQL / YDB / OceanBase notes).  
Reference implementation: [ydb-platform/tpcc-postgres-cpp](https://github.com/ydb-platform/tpcc-postgres-cpp)
(session returns incomplete futures; workflows `co_await TSuspendWithFuture`).

This document describes the required changes and the planned sequence to make
PostgreSQL and OceanBase worker paths genuinely asynchronous at the
`ITpccTransaction` boundary, and to bring YDB to the same model. It does
**not** propose adaptive `max_inflight` or changes to TPC-C workflows’
semantic steps.

## 1. Problem

Shared workflows already look async:

```text
co_await SuspendExecute(tx, context, op)
  → TSuspendWithFuture(tx.Execute(op), taskQueue, terminalId)
```

(`tpcc/transactions/workflow_util.h`)

But OceanBase and YDB adapters currently complete `Execute` / `Commit` / … by
**blocking the task-queue (scheduler) thread** before returning a ready
`TFuture`:

| Adapter | Mechanism on scheduler thread |
| --- | --- |
| PostgreSQL | Worker `ITpccTransaction` chains `PgSession` futures (no `.Get()` on the task-queue thread) |
| OceanBase | `TObSession::ExecuteQuery(…).Get()` inside `TObTpccTransaction::Execute` |
| YDB | `Session_.ExecuteQuery(…).GetValueSync()` inside `TYdbTpccTransaction::Execute` (no IO offload) |

Session layers for PG/OB already offload sync SDK calls to an `IExecutor` and
return incomplete `TFuture`s. OceanBase (and previously PostgreSQL) adapter
`.Get()` undoes that for the scheduler: `TSuspendWithFuture` sees a ready
future and never parks the coroutine.

Observable symptom (OceanBase, 1200 WH/worker, auto threads):

```text
2 threads, 100 max inflight  →  Inflight:2, ready:~11k, eff:~5%
```

`Inflight ≈ ThreadCount` even when `max_inflight` is large. The
`ComputeRunLayout` heuristic (`ceil(warehouses / 1000)` threads) only works
when many transactions can be in flight per scheduler thread — as in
tpcc-postgres-cpp.

## 2. Goals

1. **Caller of `ITpccTransaction::{Execute,ExecuteBatch,ExecuteFinalAndCommit,Commit,Rollback,Cancel,ExecuteSelect1}` MUST NOT block** waiting for DBMS IO.
2. After each incomplete future, workflows resume only via `TSuspendWithFuture` on the task queue (`coro_traits.h` invariant: do not resume task-queue coroutines on IO threads).
3. Preserve the shared semantic-op surface (`ops.h` / workflows); do not fork PG-style SQL into workflows.
4. Keep sync connectors (libpqxx, MariaDB C API) on a bounded `IExecutor` where needed.
5. Restore usefulness of mind/worker auto `threads=0` + large `max_inflight` for paced runs.
6. Same acceptance criterion for YDB (today it is worse than PG/OB).

Non-goals:

- Adaptive inflight / online tuning of concurrency.
- MariaDB `MYSQL_OPT_NONBLOCK` (optional later; not required if IO-pool + incomplete futures work).
- Rewriting workflows to take `PgSession&` again.
- Changing loader / check / schema admin paths in the first worker-focused PRs (they may keep `GetValueSync` until a follow-up).

## 3. Target model

```text
workflow          SuspendExecute / SuspendCommit
                      │
                      ▼
ITpccTransaction  returns incomplete TFuture<…>     // no .Get() / GetValueSync on task-queue
                      │
         ┌────────────┴────────────┐
         ▼                         ▼
   PgSession / TObSession     YDB SDK async API
   Submit(sync SDK) →         bridge → NTpcc::TFuture
   SetValue on IO pool
         │                         │
         └────────────┬────────────┘
                      ▼
TSuspendWithFuture    Subscribe → TaskReadyThreadSafe → resume on scheduler
```

### 3.1. Preferred adapter implementation style

**Future chaining (recommended first):** keep `ITpccTransaction` signatures
unchanged. Inside `Execute(op)`, chain `Session`/`SDK` futures with
`Subscribe` (or a small shared helper), parse results and issue the next
round-trip without blocking the caller. Continuations may run on the IO /
SDK callback thread; they MUST NOT touch task-queue coroutine state. The
outer incomplete future is what `TSuspendWithFuture` awaits.

**Optional later:** pass `ITaskQueue` + thread hint into the transaction and
implement multi-step ops as coroutines with `co_await TSuspendWithFuture`
(closer line-by-line to tpcc-postgres-cpp). Use if chaining becomes hard to
maintain for large `Execute` switches.

### 3.2. Spec clarification (docs)

Update [adapter-api.md](adapter-api.md) §4.3 so that:

- “MAY run blocking SDK IO on a bounded `IExecutor`” remains true for the
  **session / connector** layer;
- `ITpccTransaction` methods that touch the DBMS **MUST** return incomplete
  futures to the caller (MUST NOT `.Get()` / `GetValueSync()` on the
  task-queue thread).

## 4. Current-state notes per DBMS

### 4.1. PostgreSQL

- `PgSession` matches tpcc-postgres-cpp: incomplete futures + `IExecutor`.
- Worker `ITpccTransaction` in `tpcc/dbms/pgsql/tpcc_session.cpp` chains those
  session futures (`Then` / `ThenFold` in `tpcc/runtime/future_util.h`).
- Loader / check / schema admin paths MAY still wait synchronously until a
  follow-up.

### 4.2. OceanBase

- `TObSession` same shape as `PgSession`.
- Gap in `tpcc/dbms/oceanbase/tpcc_session.cpp`.
- Production symptom already observed; should follow PG closely for a thin
  second PR.

### 4.3. YDB — not OK today

- No session-level IO offload on the worker hot path.
- `GetValueSync()` in `Execute`, `Commit`, `Rollback`, and session acquire
  (`GetSession().GetValueSync()`).
- Interactive transactions and `ExecuteFinalAndCommit` fusion remain
  required ([adapter-api.md](adapter-api.md) §5.2); they must become async
  bridges, not sync wrappers.
- Treat as a separate, larger PR after PG+OB.

## 5. Work sequence

### Phase 0 — Contract and safety net (shared)

| # | Change | Outcome |
| --- | --- | --- |
| 0.1 | Clarify async caller rules in `adapter-api.md` §4.3 | Normative text matches target |
| 0.2 | Shared test helper or UT: incomplete `Execute` does not block the calling thread when session future is delayed | Guards against reintroducing `.Get()` |
| 0.3 | Document interim ops guidance: until phase 2 lands, OceanBase paced scale should not rely on auto `threads≈WH/1000`; pin `threads_per_worker` near desired concurrency. YDB remains blocking until phase 3. PostgreSQL worker path is async. | Avoid repeating the 2-thread regression on OB/YDB |

Phase 0 MAY ship as part of the first code PR or as docs-only ahead of it.

### Phase 1 — PostgreSQL worker async adapter

**Primary files:** `tpcc/dbms/pgsql/tpcc_session.cpp` (+ header if needed),
optional shared helper under `tpcc/runtime/` or `tpcc/transactions/`.

| # | Change |
| --- | --- |
| 1.1 | Inventory all scheduler-blocking `.Get()` on the `ITpccTransaction` path |
| 1.2 | Add small future map/chain helpers (error propagation, void→value) |
| 1.3 | Convert single-round-trip `Execute` arms to chain from `PgSession` |
| 1.4 | Convert multi-round-trip arms (`TReserveDistrictOrderId`, payment/delivery sequences, …) via sequential Subscribe; do **not** change workflow semantic ops unless a specific arm is unmaintainable |
| 1.5 | Async `Commit` / `Rollback` / `Cancel` / `ExecuteBatch` / `ExecuteFinalAndCommit` / `ExecuteSelect1` |
| 1.6 | Unit / executor test: with `ThreadCount=2` and delayed IO, inflight can exceed 2 (or equivalent non-blocking assertion) |
| 1.7 | Manual smoke: paced run, `threads=2`, `max_inflight=100` → `Inflight ≫ 2`, `ready` not ≈ terminal count |

**Exit criterion:** scheduler threads are free during libpqxx RTT; progress
log shows `Inflight` no longer glued to `ThreadCount` under load.

### Phase 2 — OceanBase worker async adapter

**Primary files:** `tpcc/dbms/oceanbase/tpcc_session.cpp`, reuse phase-1
helpers.

| # | Change |
| --- | --- |
| 2.1 | Same conversion as PG for all `ITpccTransaction` entry points |
| 2.2 | Leave `TObSession` Submit/`mysql_stmt_*` on IO pool as-is |
| 2.3 | Cluster smoke (e.g. 5×1200 WH): low or auto threads + `max_inflight=100` → `Inflight ≫ threads`, higher `eff`, lower `ready` |

**Exit criterion:** same as phase 1 on OceanBase; mind auto threads becomes
plausible again for paced OB runs.

### Phase 3 — YDB worker async adapter

**Primary files:** `tpcc/dbms/ydb/ydb_session.cpp`, possible thin
`NYdb`→`NTpcc::TFuture` bridge.

| # | Change |
| --- | --- |
| 3.1 | Bridge YDB async execute/commit futures to `NTpcc::TFuture` without `GetValueSync` on the task-queue thread |
| 3.2 | Rewrite `Execute` / batch / final-commit / commit / rollback on that bridge |
| 3.3 | Move session acquire off the scheduler hot path (async `Begin` / factory or executor) |
| 3.4 | Preserve `ExecuteFinalAndCommit` fusion semantics |
| 3.5 | Smoke with small `threads` and large `max_inflight` |

**Exit criterion:** YDB worker no longer blocks scheduler on `GetValueSync`
for per-op / commit paths.

### Phase 4 — Product closure

| # | Change |
| --- | --- |
| 4.1 | Re-validate mind `threads_per_worker: 0` auto + default `max_inflight=100` on PG and OB at multi-thousand WH |
| 4.2 | Optional harness warning when `Inflight` stays ≈ `ThreadCount` while `max_inflight > ThreadCount` for a sustained window (detect sync regressions) |
| 4.3 | Follow-up (optional): async-ify YDB/OB/PG check/admin helpers that still use sync waits outside the worker loop |

## 6. Suggested PR breakdown

| PR | Contents |
| --- | --- |
| Plan doc | This plan document + link from `adapter-api.md` |
| **PR A** (this work) | Phase 0 (spec wording) + Phase 1 (PostgreSQL) + shared helpers + tests |
| **PR B** | Phase 2 (OceanBase), thin diff on top of A |
| **PR C** | Phase 3 (YDB async bridge) |
| **PR D** | Phase 4 polish / optional diagnostics |

Do not combine C with A/B: YDB risk and review surface differ.

## 7. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Continuations on IO threads touch unsafe state | Only complete `TPromise` / parse local results; resume via `TSuspendWithFuture` |
| Multi-step `Execute` arms become unreadable | Shared chain helpers; fall back to TaskQueue-aware coroutine style for that arm only |
| YDB interactive tx + fused commit races | Keep fusion in one async pipeline; expand classifier tests |
| Temporary perf cliff if only half the ops are converted | Convert all `ITpccTransaction` entry points in one DBMS per PR; refuse partial “some ops still `.Get()`” |
| Auto threads still wrong if async incomplete | Keep ability to pin `threads_per_worker`; acceptance tests before declaring auto safe |

## 8. Acceptance checklist (per DBMS worker)

- [ ] No `.Get()` / `GetValueSync()` on the task-queue thread inside `ITpccTransaction` methods.
- [ ] Paced smoke with `threads=2` (or auto low) and `max_inflight≥100`: steady-state `Inflight` clearly above `ThreadCount` when the DB is not saturated.
- [ ] `Fail=0` (or only expected integrity/user-abort profile); no new permanent errors from the refactor.
- [ ] `ready` queue depth no longer tracks nearly all terminals during ramp when the DB has headroom.
- [ ] Efficiency not worse than a manually pinned high-thread sync baseline on the same cluster.

## 9. References

- `tpcc/transactions/workflow_util.h` — `SuspendExecute`
- `tpcc/runtime/coro_traits.h` — why bare `co_await TFuture` was removed
- `tpcc/runtime/future_util.h` — `Then` / `ThenFold` / `CatchToValue`
- `tpcc/dbms/pgsql/tpcc_session.cpp` — PostgreSQL async `ITpccTransaction`
- `tpcc/dbms/oceanbase/tpcc_session.cpp` / `ob_session.cpp`
- `tpcc/dbms/ydb/ydb_session.cpp` — `GetValueSync`
- tpcc-postgres-cpp `pg_session.cpp` + `transaction_neworder.cpp` — target await style
