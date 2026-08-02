# Action Plan: Align Implementation with Specification

Status: **decisions recorded** (2026-07-29). Spec doc updates land with this plan.
Depends on: [specification.md](specification.md), [adapter-api.md](adapter-api.md).
Current module status: [dependencies.md](dependencies.md). TPC-C 5.11 notes:
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md).

This plan brings `tpcc/` and `tools/tpccctl` to the architecture’s “Done When”
criteria ([specification.md](specification.md) §13).

---

## 1. Principles

1. Prefer fixing the code to match the spec; change the spec only where
   explicitly approved below.
2. Keep the PostgreSQL port runnable at every milestone (`init`/`import`/`run`
   and orchestrated `loader`/`worker`).
3. Minimize diff; no drive-by refactors; do not touch protected infrastructure
   directories unless explicitly requested.
4. After each phase: build affected targets, smoke the PG path, update this
   plan’s checklist and `dependencies.md` status tables.

---

## 2. Specification / API decisions

Stakeholder reply: **A+C+D+F+G**; **E rejected** and replaced by wall-clock
start sync (below). **B not approved** → keep `ExecuteBatch` on the normative
session surface (MAY be emulated as repeated `Execute`).

| ID | Decision |
| --- | --- |
| **A** | **Accepted.** Session API is async (`TFuture`). Open decision §14.1 → `TFuture`. |
| **B** | **Not accepted.** `ExecuteBatch` remains in the API; adapters MAY emulate. |
| **C** | **Accepted.** Semantic ops = tagged structs / `std::variant` in `ops.h`. |
| **D** | **Accepted.** Normative roles `schema\|loader\|worker\|check`; legacy aliases OK. |
| **E** | **Rejected.** No DB fence / file control-lock staging. Replaced by **§2.1**. |
| **F** | **Accepted.** Delivery MAY be sync when `async_delivery = false`. |
| **G** | **Accepted.** Exact decimal stays MUST; implement in Phase 1 with generator. |

### 2.1. Wall-clock start synchronization (replaces proposal E / DB fence)

**Assumption:** hosts keep system clocks tightly synchronized (NTP/chrony or
equivalent). `phases.max_clock_skew_ms` records the allowed skew budget for
validation and status; the tool does not implement its own clock sync.

**Protocol:**

1. `tpccctl` distributes the same `run-config.json` and launches each
   `loader` / `worker` with an absolute start instant on the command line,
   e.g. `--start-at=2026-07-28T12:00:15Z` (UTC, RFC 3339).
2. The orchestrator chooses `--start-at` as **now + `phases.start_lead`**
   (and any other configured launch/init margin), large enough for remote
   process start and local prepare (connect, pools, terminals) on all hosts.
3. Each process **MUST** finish prepare before `--start-at`. Until that
   instant it waits (does not admit workload transactions).
4. If wall-clock time reaches `--start-at` before the process is ready to
   admit work, the process **MUST** exit fatally with a clear error
   (missed start deadline).
5. If any required process exits fatally before / at start (including missed
   deadline), `tpccctl` **MUST** abort the run and stop the remaining
   processes.
6. Phase schedule is derived from `--start-at` plus durations in run-config:
   `ramp_start = start-at`, then `measurement_start/end`, drain deadlines.
7. The orchestrator **SHOULD** record the chosen schedule (e.g.
   `start-token.json` or equivalent under `orchestrator/`) in the result
   package for audit; workers do **not** wait on a readiness barrier file
   before starting—the wall-clock instant is the sync point.

**Non-goals:** DB-scoped fence and cross-control mutual exclusion are **not**
required. Operators MUST NOT run two control processes against the same
database path concurrently; the product does not enforce that via DBMS
metadata.

**Configurable margins:** `phases.start_lead` (profile / run-config
`start_lead_ms`) is the primary launch+init budget. Additional stop/drain
fields remain as today (`transaction_drain`, `stop_grace`, optional async
drain when `async_delivery` is true).

---

## 3. Phased implementation plan

### Phase 0 — Hygiene and contracts

| # | Task | Notes |
| --- | --- | --- |
| 0.1 | Align `transactions/session.h` with decisions A/B/C (async + variant ops + batch) | Spec already updated |
| 0.2 | Document known defects in `dependencies.md` | Done (see Remaining work) |
| 0.3 | Add/extend unit tests for assignment, histogram merge helpers, run-config parse | Go + C++ as available |

**Exit:** headers and docs agree; no full behavior migration yet.

---

### Phase 1 — Deterministic data & load foundation **[G]**

Goal: same `run-config` (scale, seed) → same logical rows; load retries safe.

| # | Task | Location |
| --- | --- | --- |
| 1.1 | Seeded RNG API in `tpcc/domain` | `domain/` |
| 1.2 | Create `tpcc/generator`: population + per-tx inputs; timestamps from seed/config | `generator/` |
| 1.3 | Create `tpcc/loader` helpers: batch identity → `PutBatch` | `loader/` |
| 1.4 | PG: idempotent `PutBatch`; `OwnsGlobalData` | `dbms/pgsql/` |
| 1.5 | PG: no wall-clock load dates; fix multi-loader preflight race | import / path_checker / tpccctl |
| 1.6 | Exact decimal (`TMoney` or equivalent) + PG `c_ytd_payment` → `DECIMAL` | domain + init + binds |
| 1.7 | Apply `data.seed`, `batch_rows` | `run_config.*` |

**Exit:** shard reload idempotent; fixed seed → stable logical content.

---

### Phase 2 — Session / workflows / errors **[A][C]** (**B** kept in API)

| # | Task | Depends | Status |
| --- | --- | --- | --- |
| 2.1 | Finalize async `session.h` + `ops.h` (`std::variant` semantic ops) | A, C | Done (Phase 0/1) |
| 2.2 | PG `IErrorClassifier` (SQLSTATE); honor `retry.*`; no blind retry on ambiguous commit | | Done |
| 2.3 | PG `ISessionFactory` wrapping `PgSession` (`TFuture`) | 2.1 | Done |
| 2.4 | Lift five workflows to `tpcc/transactions` | 2.1, 1.2 | Done |
| 2.5 | Terminal retry loop + attempt metrics | 2.2 | Done |
| 2.6 | `ICapabilities` including `async_delivery` **[F]** | | Done (`TPgCapabilities`) |

**Exit:** workflows no longer take `PgSession&`.

---

### Phase 3 — Runtime phases, metrics, worker artifacts **[F][§2.1]**

| # | Task | Notes | Status |
| --- | --- | --- | --- |
| 3.1 | Shared phase controller; stop admission at measurement end | | Done |
| 3.2 | Honor `--start-at`; wait until instant; fatal if prepare late | Wall-clock sync | Done |
| 3.3 | Derive phase timestamps from `start-at` + run-config durations | | Done |
| 3.4 | Fix warmup exclusion; measurement ends at `measurement_end` | | Done |
| 3.5 | Drain = in-flight only when `async_delivery=false` **[F]** | | Done |
| 3.6 | `result.json`: raw histogram buckets, retries, versions; no final percentiles | | Done |
| 3.7 | Apply full `workload.*` / histogram / pacing from run-config | | Done |
| 3.8 | Stable process nonce; prepare artifacts for diagnostics | | Done |

**Exit:** workers sharing the same `--start-at` share phase boundaries;
missed deadline fails the process; consolidate can merge raw histograms.

---

### Phase 4 — Checks & admin surface **[D]** ✅

| # | Task | Notes |
| --- | --- | --- |
| 4.1 | Shared check catalog in `tpcc/checks` | Done |
| 4.2 | PG `ICheckAdapter` → structured `checks/` JSON | Done |
| 4.3 | Fix check SQL blind spots | Done (OUTER JOIN / NULL-safe / NULL-only carrier / mixed delivery) |
| 4.4 | Normative roles + legacy aliases **[D]** | Done (`schema\|loader\|worker\|check`; `init`≡`schema`) |
| 4.5 | `EnsureSchema` / indexes / stats / `Clean` / `Describe` | Done (`TPgAdminAdapter`; no fence APIs) |
| 4.6 | Reject unknown `database.options`; close password/`endpoint` bypass | Done |

**Exit:** orchestrated after-import / after-run checks are machine-readable.

---

### Phase 5 — `tpccctl` orchestration **[§2.1]** ✅

| # | Task | Notes |
| --- | --- | --- |
| 5.1 | SSH deploy of binary + `run-config.json`; host-key policy | Done (`internal/remote`; local loopback + SSH/known_hosts) |
| 5.2 | Launch/supervise via `process.json` | Done (detached start + artifact-manifest wait) |
| 5.3 | Compute `--start-at = now + start_lead`; pass to all workers; record schedule | Done (`start-token.json`; no ready-barrier) |
| 5.4 | On any required process fatal before measurement → abort run / stop peers | Done |
| 5.5 | `collect` + layout per §8 | Done (`raw/`, `checks/`, `orchestrator/`) |
| 5.6 | `consolidate`: merge buckets then percentiles; embed settings; real status | Done |
| 5.7 | Full `run` pipeline + skip recording; strict validate | Done (`check_after_run`, skipped_steps, pgsql options/known_hosts) |

**Exit:** multi-host PG run via `tpccctl run` yields valid `aggregate.json`.

---

### Phase 6 — Open decisions, tests, other DBMS

| # | Task |
| --- | --- |
| 6.1 | Histogram layout / max latency (§14.2) |
| 6.2 | Ambiguous-commit policy per DBMS (§14.3) |
| 6.3 | Canonical row bytes — implement or explicitly defer (§14.4) |
| 6.4 | Minimum PG version in capabilities / docs (§14.5) |
| 6.5 | Port unit/integration tests from upstream PG port |
| 6.6 | YDB / OceanBase against frozen session API |

---

## 4. Sequencing

```text
Phase 0 (headers/docs)
    │
    ├─► Phase 1 (generator + idempotent load + decimals)
    │       └─► Phase 2 (async session + variant ops + workflows)
    │               └─► Phase 3 (--start-at phases + histograms)
    │
    ├─► Phase 4 (checks + admin + aliases)   [parallel with 1–3]
    │
    └─► Phase 5 (tpccctl) ── needs 3.2/3.3 + 4.4 + 1.4
            └─► Phase 6
```

---

## 5. Out of scope

- Official TPC certification / edition conformance verdicts.
- DBMS cluster provisioning, K8s/Ansible.
- DB-scoped fence / multi-control mutual exclusion.
- YDB/OceanBase before Phase 6.
- Modifying `build/`, `contrib/`, `devtools/`, `library/`, `util/` unless
  explicitly requested.

---

## 6. Immediate next step

Phase 0–5 scaffolding complete for `tpccctl` (SSH/local remote drive,
`--start-at`, collect/consolidate with histogram merge). Next: harden
multi-host SSH runs in the field and **Phase 6** (open decisions, tests,
other DBMS).
