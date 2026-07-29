# Action Plan: Align Implementation with Specification

Status: proposed plan (awaiting confirmation on API/spec adjustments in §2).
Depends on: [implementation-gap-analysis.md](implementation-gap-analysis.md),
[specification.md](specification.md), [adapter-api.md](adapter-api.md).

This plan brings `tpcc/` and `tools/tpccctl` to the architecture’s “Done When”
criteria ([specification.md](specification.md) §13). Work that follows the
current docs proceeds without waiting. Items that would **change** the
normative API are listed in §2 and **must not start until explicitly approved**.

---

## 1. Principles

1. Prefer fixing the code to match the spec; change the spec only where it
   reduces sync cost without losing multi-host correctness or adapter isolation.
2. Keep the PostgreSQL port runnable at every milestone (`init`/`import`/`run`
   and orchestrated `loader`/`worker`).
3. Minimize diff; no drive-by refactors; do not touch protected infrastructure
   directories unless explicitly requested.
4. After each phase: build affected targets, smoke the PG path, update this
   plan’s checklist and `dependencies.md` status tables.

---

## 2. Proposed specification / API adjustments (NEED CONFIRMATION)

Do **not** implement these until you approve each proposal (approve all,
subset, or none). Rejected items stay as current-spec implementation work.

### Proposal A — Async-first session API (coroutine/`TFuture`)

**Today (spec):** synchronous `ITpccTransaction::Execute*` returning
`TOperationResult` / `TCommitResult` ([adapter-api.md](adapter-api.md) §4.3).
Open decision §14.1 notes coroutine/future ABI is unresolved.

**Today (code):** `PgSession` is entirely `TFuture`-based; terminals are
coroutines. A sync abstract API forces a second wrapper layer and either
blocks scheduler threads or invents a sync-over-async bridge.

**Proposed change:**

- Make the normative session surface **async**: methods return `TFuture<…>`
  (or an equivalent shared future type named in the spec).
- Keep the same logical operations: `Begin`, `Execute`, `ExecuteBatch`,
  `ExecuteFinalAndCommit`, `Commit`, `Rollback`, `Cancel`.
- Document that adapters MAY run blocking SDK IO on a bounded `IExecutor`
  (current PG pattern).
- Resolve open decision §14.1 in favor of “shared libraries use `TFuture` as
  the ABI”.

**Why:** removes the largest impedance mismatch before lifting workflows out
of `dbms/pgsql`.

**Docs to edit if approved:** `adapter-api.md` §4.3 / §4.3.4,
`specification.md` §4.2 and §14.1, `transactions/session.h` shape.

---

### Proposal B — Defer `ExecuteBatch` as required; keep `ExecuteFinalAndCommit` as MAY-with-emulation

**Today:** both primitives are part of the target surface; batch is for
set-oriented adapters; fused commit is “required for efficient YDB”, optional
elsewhere.

**Proposed change:**

- Phase 1 shared workflows only require `Execute` + `Commit`/`Rollback`/
  `Cancel` (+ `Begin`).
- `ExecuteFinalAndCommit`: remain in the API; default emulation =
  `Execute` then `Commit` (already allowed). YDB must override later.
- `ExecuteBatch`: mark **optional until the first adapter that needs it**
  (YDB/OceanBase). Shared workflows MUST NOT call it until then; when added,
  it is an optimization only (same logical steps).

**Why:** PG workflows are statement-at-a-time; designing typed batch ops now
delays the migration without benefiting PG.

**Docs to edit if approved:** `adapter-api.md` §4.3.2 (status of batch),
capabilities flags already anticipated — make “batch unsupported” a normal
capability, not a missing interface.

---

### Proposal C — Semantic ops as tagged structs + visitor/dispatch (not `void*`)

**Today:** skeleton uses `ExecuteSemantic(const void*, size_t)`; docs speak of
typed semantic operations without specifying the C++ encoding.

**Proposed change (concrete encoding):**

- Define a closed set of operation structs in `tpcc/transactions/ops.h`
  (e.g. `TGetCustomerById`, `TUpdateStock`, …) with exact domain types.
- `Execute(const TSemanticOp&)` where `TSemanticOp` is a `std::variant` of
  those structs (or a base + `enum` tag + payload).
- Adapters switch on the tag and bind SQL/SDK calls.
- Result payloads: typed fields inside `TOperationResult` via variant, or
  out-params filled by the adapter for ops that return rows.

**Why:** makes the shared/adapter boundary compile-time checkable; replaces
the placeholder `void*` without inventing a full ORM.

**Docs to edit if approved:** `adapter-api.md` §4.3.1 and §6 (add encoding
rule); drop `void*` from the skeleton.

---

### Proposal D — Keep legacy CLI aliases; require orchestrated role names

**Today:** binary roles MUST be `schema | loader | worker | check`
([specification.md](specification.md) §4.3, adapter-api §7). Code has
`init` / `import` / `run` / `clean` / `check` plus `loader` / `worker`.

**Proposed change:**

- Orchestrator and docs treat **`schema` / `loader` / `worker` / `check`** as
  the normative remote roles.
- Legacy aliases remain supported indefinitely for local use:
  - `init` ≡ `schema` (possibly with drop/recreate semantics documented)
  - `import` ≡ standalone load (flags, not run-config)
  - `run` ≡ standalone worker (flags, not run-config)
  - `clean` remains local-only admin (not a remote role)
- Spec text: “binaries MUST implement the four roles; MAY keep standalone
  aliases.”

**Why:** avoids breaking the existing PG operator workflow while aligning
`tpccctl` argv.

**Docs to edit if approved:** `specification.md` §4.3, `adapter-api.md` §7,
`dependencies.md` CLI notes.

---

### Proposal E — Soften fence to “control lock”: file lock first, DB fence as SHOULD

**Today:** DB-scoped fence outside benchmark tables is required
([specification.md](specification.md) §7).

**Proposed change:**

- **MUST:** exclusive control lock so two `tpccctl` processes cannot drive the
  same run path / database path concurrently.
- **Phase 1:** filesystem lock under state/artifact dirs (extend today’s
  profile lock) is sufficient for single-control-host deployments.
- **SHOULD / Phase 2:** adapter `AcquireFence` / `ReleaseFence` in DBMS
  metadata for multi-control-host or shared-DB safety.
- Spec wording: replace hard MUST-on-DB-fence with MUST-on-control-lock +
  SHOULD DB fence, or explicitly stage it.

**Why:** unblocks orchestrator correctness without designing per-DBMS metadata
tables first.

**Docs to edit if approved:** `specification.md` §7, `adapter-api.md` §4.1.

---

### Proposal F — Async Delivery: allow sync Delivery for PG until shared runtime lands

**Today:** bounded drain for async Delivery-style work is part of runtime and
phases.

**Proposed change:**

- Document that Delivery **MAY** execute inline in the terminal (current PG
  behavior) when `capabilities.async_delivery = false`.
- `async_work_drain_ms` is a no-op in that mode; phase drain only waits for
  in-flight terminal work after admission stop.
- Shared async Delivery queue becomes required when the first adapter sets
  `async_delivery = true` (or in a later milestone called out in the plan).

**Why:** matches current PG port; avoids a large runtime rewrite before
phase/start-token correctness.

**Docs to edit if approved:** `specification.md` §7, `adapter-api.md` §4.6
capabilities, phase semantics.

---

### Proposal G — Exact decimals: keep MUST, allow staged rollout

**Not a relaxation of the rule** — money MUST NOT stay on `double` forever.

**Proposed staging (confirm process, not the rule):**

1. Introduce `TMoney` / fixed-point (or decimal) in `tpcc/domain`.
2. Fix PG DDL (`c_ytd_payment` → `DECIMAL`) and bind paths in the same
   milestone as generator lift.
3. Until that milestone, mark money-as-double as an explicit known defect in
   `dependencies.md` (already implied).

No spec text change required if you only confirm the **ordering** in §3.

---

### Confirmation checklist

Please reply with approvals, e.g. `A+B+C+D+E+F` / `reject E` / etc.:

| ID | Summary | Default if no answer |
| --- | --- | --- |
| A | Async `TFuture` session API | Keep sync API; wrap PG |
| B | Defer required `ExecuteBatch` | Implement batch in interface now |
| C | `variant`/tagged semantic ops | Design ops when lifting workflows |
| D | Legacy CLI aliases allowed | Rename/require only four roles |
| E | File control lock first; DB fence SHOULD | Implement DB fence with admin API |
| F | Sync Delivery allowed via capability | Build async Delivery before phase work |
| G | Exact decimal after/with generator | Same (ordering only) |

---

## 3. Phased implementation plan

Phases are ordered by dependency. Items marked **[spec?]** wait on §2.
Items without that mark can start under the **current** specification.

### Phase 0 — Hygiene and contracts (short)

| # | Task | Notes |
| --- | --- | --- |
| 0.1 | Fix `transactions/session.h` includes / compile as header-only | Independent of A–C |
| 0.2 | Document known defects in `dependencies.md` (seed unused, percentiles in workers, …) | Point to gap analysis |
| 0.3 | Add/extend unit tests for assignment, histogram merge helpers, run-config parse | Go + C++ as available |
| 0.4 | Resolve §2 proposals with stakeholder | **Gate for 2.x / 4.x API work** |

**Exit:** plan confirmed; no behavior change required.

---

### Phase 1 — Deterministic data & load foundation

Goal: same `run-config` (scale, seed) → same logical rows; load retries safe.

| # | Task | Location |
| --- | --- | --- |
| 1.1 | Seeded RNG API in `tpcc/domain` (replace thread-local random_device for load/txn inputs) | `domain/` |
| 1.2 | Create `tpcc/generator`: population rows + per-tx logical inputs; timestamps derived from seed/config, not wall clock | `generator/` |
| 1.3 | Create `tpcc/loader` helpers: batch identity (`run_id` + table + key range), call adapter `PutBatch` | `loader/` |
| 1.4 | PG: implement idempotent `PutBatch` (COPY to staging + replace range, or upsert); honor `OwnsGlobalData` | `dbms/pgsql/` |
| 1.5 | PG: stop using wall-clock for initial `c_since` / order dates; remove multi-loader “empty warehouse” race (preflight once / orchestrator-ordered) | `import.*`, `path_checker.*`, `tpccctl` |
| 1.6 | Exact decimal type + PG DDL fix for `c_ytd_payment` **[G ordering]** | `domain/`, `init.cpp`, bind paths |
| 1.7 | Parse and apply `data.seed`, `batch_rows` | `run_config.*` |

**Exit:** reload of same shard is idempotent; two adapters (when they exist)
would agree on logical content for a fixed seed; post-import cardinality
checks still pass.

---

### Phase 2 — Session / workflows / errors **[A][B][C]**

Goal: shared workflows; PG is an adapter behind the abstract session API.

| # | Task | Depends |
| --- | --- | --- |
| 2.1 | Finalize `session.h` per approved A/B/C | §2 |
| 2.2 | `IErrorClassifier` for PG (SQLSTATE → `EErrorClass`); honor `retry.*` from run-config; no blind retry on `ambiguous_commit` | current spec |
| 2.3 | Implement PG `ISessionFactory` / transaction wrapping `PgSession` | 2.1 |
| 2.4 | Lift five workflows to `tpcc/transactions` over semantic ops | 2.1, 1.2 |
| 2.5 | Terminal retry loop uses normalized errors + metrics for attempts | 2.2 |
| 2.6 | `ICapabilities` stub recorded into result settings | current spec |

**Exit:** `transaction_*.cpp` SQL stays in adapter bind layer; workflows no
longer take `PgSession&`.

---

### Phase 3 — Runtime phases, metrics, worker artifacts

Goal: synchronized measurement; mergeable histograms; correct drain.

| # | Task | Notes |
| --- | --- | --- |
| 3.1 | Shared phase controller: prepare → ramp-up → measurement → drain; stop admission at measurement end | Move logic from `runner.cpp` |
| 3.2 | Consume `start-token.json` (absolute timestamps); refuse work before token | Spec §7 |
| 3.3 | Fix warmup exclusion (no racy per-terminal clear); measurement window ends at `measurement_end` | |
| 3.4 | Drain = finish in-flight (+ async queue if **[F]** rejected/approved accordingly) | |
| 3.5 | Worker `result.json`: raw histogram buckets, counters, retries, versions; **no** authoritative percentiles | Spec §8 |
| 3.6 | Apply `workload.*`, histogram, pacing, terminals-per-WH from run-config | |
| 3.7 | Write `ready.json` only after successful prepare; stable process nonce across artifacts | |

**Exit:** two workers with the same token share phase boundaries; consolidate
can recompute percentiles from raw files alone.

---

### Phase 4 — Checks & admin surface **[D][E]**

| # | Task | Notes |
| --- | --- | --- |
| 4.1 | Shared check catalog IDs + result schema in `tpcc/checks` | Lift from `check.cpp` |
| 4.2 | PG `ICheckAdapter` evaluators; structured JSON under `checks/` | |
| 4.3 | Fix known check SQL blind spots (empty groups, mixed delivery dates) | Gap analysis §6 |
| 4.4 | `schema` / `check` run-config roles; aliases per **[D]** | |
| 4.5 | `EnsureSchema` / `EnsureIndexes` / `EnsureStatistics` / `Clean` / `Describe` | Order: indexes then ANALYZE |
| 4.6 | Control lock **[E]**; DB fence if E rejected or in Phase 4b | |
| 4.7 | Reject unknown `database.options`; close password/`endpoint` bypass | Secrets §9 |

**Exit:** after-import / after-run checks are machine-readable and orchestrated.

---

### Phase 5 — `tpccctl` orchestration

| # | Task | Notes |
| --- | --- | --- |
| 5.1 | SSH deploy of binary + `run-config.json`; host-key policy from profile | Spec §3 / §9 |
| 5.2 | Launch/supervise loader & worker via `process.json`; stop without killing wrong PID | |
| 5.3 | Readiness barrier → issue `start-token.json` with absolute phase schedule | |
| 5.4 | `collect` verified pull of raw artifacts; layout per §8 | |
| 5.5 | `consolidate`: merge histogram buckets then percentiles; embed full settings; real status flags | |
| 5.6 | `run` pipeline including after-run check; record skips in run-state / aggregate | |
| 5.7 | Strict validate (unknown fields, positive sizes, mix, no secret literals) | Spec §10 |

**Exit:** multi-host PG run through `tpccctl run` produces a valid
`aggregate.json` without manual SSH.

---

### Phase 6 — Close “Done When” and open decisions

| # | Task |
| --- | --- |
| 6.1 | Histogram bucket layout / max latency decided and encoded in run-config + docs (§14.2) |
| 6.2 | Per-DBMS ambiguous-commit policy documented and tested (§14.3) |
| 6.3 | Canonical row bytes for sample checks — implement or explicitly defer (§14.4) |
| 6.4 | Minimum PG version recorded in capabilities / docs (§14.5) |
| 6.5 | Port unit/integration tests from upstream PG port |
| 6.6 | Only then: YDB / OceanBase adapters against the frozen session API |

---

## 4. Suggested sequencing (dependency graph)

```text
Phase 0 (hygiene + approve §2)
    │
    ├─► Phase 1 (generator + idempotent load + decimals)
    │       │
    │       └─► Phase 2 (session API + workflows + errors)  [needs A/B/C]
    │               │
    │               └─► Phase 3 (phases + histograms + run-config workload)
    │                       │
    ├─► Phase 4 (checks + admin + secrets)  [D/E parallel with 1–3 where possible]
    │
    └─► Phase 5 (tpccctl) ── needs 3.2 artifacts + 4.4 roles + 1.4 loaders
            │
            └─► Phase 6 (open decisions + tests + other DBMS)
```

Parallelism: Phase 4 admin/check catalog and Phase 1 generator can proceed
in parallel after Phase 0. Phase 5 should not be considered done before
Phase 3 worker artifacts exist.

---

## 5. Out of scope (unchanged)

- Official TPC certification / edition conformance verdicts.
- DBMS cluster provisioning, K8s/Ansible.
- Implementing YDB/OceanBase before the shared API stabilizes (Phase 6).
- Modifying `build/`, `contrib/`, `devtools/`, `library/`, `util/` unless
  explicitly requested (e.g. libpqxx upgrade).

---

## 6. Immediate next step

1. **You confirm §2 proposals** (A–G).
2. On confirmation, edit the affected design docs in one small PR, then start
   Phase 0.4 → Phase 1 implementation PRs.
3. If all API proposals are rejected, Phase 1 and most of Phase 3/5 still
   proceed; Phase 2 follows the current sync session surface with a
   sync-over-async adapter around `PgSession`.
