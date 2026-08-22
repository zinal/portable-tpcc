# portable-tpcc Specification

Status: architecture draft, version 1.

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
are to be interpreted as described in RFC 2119.

## 1. Purpose

`portable-tpcc` is a horizontally scalable TPC-C-style workload generator for
multiple DBMSs. The product consists of:

1. shared C++ libraries with the workload model, generator, terminal runtime,
   metrics, and integrity checks;
2. one DBMS adapter and one `tpcc-<dbms>` binary per supported DBMS;
3. the Go orchestrator `mind-tpcc`.

Shipped binaries: `mind-tpcc`, `tpcc-ydb`, `tpcc-pgsql`, `tpcc-oceanbase`.
There is no extra DBMS-neutral helper binary.

Initial adapters: YDB, PostgreSQL, OceanBase. Adding a DBMS MUST NOT require
forking the shared workload logic.

This document does not restate the TPC-C standard. The project implements one
fixed workload model. Run parameters (transaction mix, think/keying times,
terminals per warehouse, and similar) are defaults in `mind-tpcc` and MAY be
overridden in the profile. The tool does not select TPC-C editions and does
not emit an official TPC-C conformance verdict. It DOES report soft deviations
of effective launch parameters from the fixed TPC-C 5.11 requirements used by
the built-in defaults (validate output, test warning, aggregate status).

Results MUST NOT be called official TPC-C results without independent TPC
verification. By default the report uses `result_class: engineering`.

## 2. Scope

### 2.1. Goals

- One logical database, many generator hosts.
- Clear warehouse ownership for loaders and workers.
- The same logical data and transaction inputs on every adapter.
- Synchronized ramp-up, measurement, and drain.
- Correct merge of counters and histograms (no averaging of percentiles).
- A self-contained result package that includes the concrete settings used.
- Basic integrity and infrastructure checks.

### 2.2. Non-goals

- DBMS cluster provisioning.
- Mandatory Kubernetes / Ansible / systemd.
- A universal SQL dialect or portable stored-procedure interface.
- Automatic TPC certification.
- Proving eight-hour sustained operation or producing a TPC Full Disclosure
  Report, pricing/availability statements, and mandatory disclosure graphs.
  These are responsibilities of the test operator and independent verification
  process; the tooling provides engineering workload and source artifacts.
- A full Remote Terminal Emulator with TPC-C menu/screens and end-user
  response-time semantics. Latency is measured at the workload-client boundary.
- Built-in TPC-C atomicity, isolation, durability, power-loss, or other
  certification tests. Transaction implementations are still required to use
  DBMS mechanisms that provide the consistency guarantees defined below.
- Issuing, scheduling, or verifying DBMS checkpoints. Checkpoint mechanisms
  differ between DBMSs and remain an operator/DBMS responsibility.
- Version negotiation or compatibility between worker artifacts produced by
  different portable-tpcc versions. A run uses one operator-deployed,
  homogeneous artifact set.
- Continuing measurement after a worker is lost.
- Dynamic terminal rebalancing during measurement.
- Multi-edition TPC-C support or an official TPC-C conformance / certification
  verdict (soft launch-parameter deviation reporting is in scope; see §10).
- A separate `tpcc-spec`-style helper binary.
- Using hashes of config files as a substitute for embedding those configs in
  the final result.

## 3. Architecture

```text
          mind-tpcc (control host)
               |  SSH
     +---------+---------+
     |                   |
  loaders             workers
     |                   |
     +---------+---------+
               |
        one logical DB
```

| Role | Count | Responsibility |
| --- | ---: | --- |
| control | 1 | `mind-tpcc`: plan, deploy, drive phases, collect, merge |
| db | 1 | pre-provisioned DBMS |
| loader | 1..N | non-overlapping load shards |
| worker | 1..N | terminals for assigned warehouses |

## 4. Components

### 4.1. Shared libraries

Linked into every `tpcc-<dbms>` binary:

- domain types and exact decimals (no `double` in the domain/adapter API);
- generator (data + transaction inputs);
- transaction workflows over an abstract session API;
- runtime (terminals, pacing, retry, phases, metrics);
- loader helpers;
- integrity check catalog (not TPC-C edition compliance).

### 4.2. Adapter API

Each `tpcc/dbms/<name>` implements admin, load (`PutBatch`), session/factory,
checks, error classification, and capabilities. SDK types stay inside the
adapter.

Session surface (async; methods return `TFuture<…>` — see
[adapter-api.md](adapter-api.md) §4.3):

```text
Begin / Execute / ExecuteBatch / ExecuteFinalAndCommit
Commit / Rollback / Cancel / ExecuteSelect1
```

Worker methods that touch the DBMS MUST return a future that MAY still be
incomplete when the method returns. The caller MUST NOT block waiting for
DBMS IO. Adapters MUST NOT complete those methods with `.Get()` /
`GetValueSync()` (or equivalent) on the task-queue thread. Blocking SDK IO
MAY run on a bounded `IExecutor` at the **session / connector** layer
(`PgSession`, `TObSession`) or via the YDB SDK async API; that offload does
not license the `ITpccTransaction` wrapper to wait on the scheduler.
Workflows resume incomplete futures only via `TSuspendWithFuture` on the
task queue. PostgreSQL, OceanBase, and YDB worker paths follow this rule.

Semantic operations are a closed set of tagged structs (`std::variant` or
equivalent), not opaque `void*` blobs.

Details, module boundaries, PostgreSQL reference mapping, and DBMS-specific
guidance (YDB fused commit, DDL/query differences, OceanBase notes) are in
[adapter-api.md](adapter-api.md).

### 4.3. Binaries

`mind-tpcc` orchestrates. Each `tpcc-<dbms>` binary **MUST** expose the
normative roles `schema`, `loader`, `indexes`, `worker`, and `check`.
Non-DBMS logic comes from shared libraries; only the adapter/driver is
DBMS-specific.

Binaries **MAY** keep standalone aliases for local use (`init` ≡ `schema`,
`import` / `run` with flag-driven config, `clean` as local admin).
Orchestrated workload remotes use the five normative role names
(`schema`, `loader`, `indexes`, `worker`, `check`). `mind-tpcc cleanup`
**MAY** also launch `clean --run-config --instance` as an admin helper to
drop TPC-C objects with the same secret handling as other roles.

## 5. Configuration Model

Human-edited input: one YAML profile (`portable-tpcc/v1`).

`mind-tpcc` validates the profile, fills built-in defaults for omitted workload
fields, computes warehouse assignment, and writes a normalized
`run-config.json`. That file is the only declarative input distributed to
loaders and workers.

`run-config.json` includes concrete values, not hash stand-ins for other
documents:

- `run_id`, DBMS settings (no passwords or tokens — only worker-local paths
  such as `password_file` / `sa_key_file` / `ca_file`, or a `password_env`
  name for standalone);
- scale, seed, workload (mix, think/keying times, terminals per warehouse);
- loader/worker instance lists and computed warehouse ranges;
- phase durations and runtime/retry/histogram settings;
- binary name used for the run (for example `tpcc-ydb`).

Assignment algorithm (`balanced-contiguous`): sort instance names
bytewise ascending; split warehouses into contiguous ranges; give remainder
warehouses to the first instances; DB-wide data belongs to the first loader.
No manual ranges in the profile. A warehouse's home terminals MUST NOT be
split across workers.

Mix weights MUST be positive and form a complete distribution. Values that
differ from any TPC-C edition are still accepted.

Examples: [profile.ydb.v1.yaml](examples/profile.ydb.v1.yaml),
[run-config.v1.json](examples/run-config.v1.json).

## 6. Load

Loaders read `run-config.json` and generate deterministic batches from the
shared libraries.

`PutBatch` is idempotent: retries of the same batch for the same `run_id`
MUST leave the same final rows. Rows are fully determined by the run-config
(scale, seed, …). Unconfirmed batches are retried; there is no separate
recovery mode.

Date/time fields in the initial population intentionally use a deterministic
synthetic reference time derived from the fixed generator epoch and seed,
rather than the loader host wall clock. Individual row timestamps may be
deterministically distributed inside the generator's fixed reference window.
This models a load scenario anchored at one concrete reference date while
preserving byte-for-byte stable population across hosts, retries, and adapters.

Exactly one loader owns DB-wide tables; others own disjoint warehouse ranges.

After load, on a quiet database, run integrity checks: assignment coverage,
expected cardinalities for the configured scale/seed, sample content checks
shared across adapters, and DBMS-specific index/stats readiness.

## 7. Workload Execution

Workers build terminals for their warehouse ranges from the run-config.
Logical inputs are fixed before the first attempt and MUST NOT be regenerated
on retry.

Admission uses `max_inflight` (profile `runtime.max_inflight_per_worker`,
standalone `--max-inflight`). Scheduler `threads` (`runtime.threads_per_worker`
/ `--threads`) run terminal coroutines. When `ITpccTransaction` returns
incomplete futures, many transactions MAY be in flight per scheduler thread
(up to `max_inflight`). Completing DBMS IO on the task-queue thread makes
`Inflight ≈ ThreadCount` and MUST NOT be used on the worker path
([adapter-api.md](adapter-api.md) §4.3). Auto `threads=0`
(`ComputeRunLayout`, ≈ `ceil(warehouses / 1000)` plus CPU caps) assumes
that model. Workers MAY log a one-shot warning when progress `Inflight`
stays near `ThreadCount` while `max_inflight` is larger and the scheduler
ready queue is backlogged.

Normalized errors: `retryable_abort`, `not_committed`, `ambiguous_commit`
(no blind retry), `permanent`, `integrity` (fail the run), `cancelled`.

Phases: prepare → ramp-up → measurement → drain. Absolute phase instants are
derived from a wall-clock `--start-at` plus durations in the run-config.
Warmup samples do not enter measurement metrics.

Delivery work intentionally runs synchronously, inline in the terminal. All
adapters **MUST** report `async_delivery = false`; there is no deferred
Delivery queue or Delivery result log, and `async_work_drain` is a no-op.
This is a deliberate workload-model deviation from official TPC-C, chosen to
keep the benchmark focused on DBMS transaction execution.

The product does not emulate TPC-C menus or screens. Keying/think pacing is a
workload-generation mechanism, and recorded latency covers the workload-client
request boundary rather than complete end-user Remote Terminal Emulator
response time. Keying time is constant. Think time defaults to the TPC-C
negative-exponential distribution (`Tt = -log(r) * mean`, truncated at
`10 * mean`). Profile field `runtime.think_time_distribution: compatibility`
(alias `constant`) MAY select fixed mean think time for engineering
comparability with historical portable-tpcc runs.

Every adapter **MUST** execute each shared transaction workflow with atomic
commit/rollback semantics and an isolation level sufficient to preserve the
logical consistency conditions under concurrent home and remote transactions.
The adapter **MUST** classify transaction/commit errors according to the
normalized error model and **MUST NOT** expose a successful result before the
commit outcome is known. These guarantees are implementation requirements even
though portable-tpcc does not contain the official TPC-C ACID certification
test procedures.

The runtime does not issue or inspect DBMS checkpoints. Operators are
responsible for configuring transaction logs, checkpoints, recovery, and
durability mechanisms appropriate to the selected DBMS and test objective.

Startup (wall-clock rendezvous):

1. Distribute the same `run-config.json` to all hosts.
2. `mind-tpcc` chooses an absolute UTC start instant
   `--start-at = now + phases.start_lead` (plus any other configured
   launch/init margin) large enough for remote start and local prepare on
   every host. Hosts are assumed to keep system clocks tightly synchronized;
   `phases.max_clock_skew_ms` records the skew budget for status/validation.
3. Launch each required process with that `--start-at` on the command line.
4. Each process prepares (connect, pools, terminals, …). Until `--start-at`
   it MUST NOT admit workload transactions.
5. If wall-clock time reaches `--start-at` before the process is ready to
   admit work, the process MUST exit fatally (missed start deadline).
6. If any required process exits fatally before measurement (including a
   missed deadline), `mind-tpcc` MUST abort the run and stop remaining
   processes.
7. At `--start-at`, workers begin ramp-up; later phase boundaries follow
   run-config durations. The orchestrator SHOULD record the chosen schedule
   under `orchestrator/` (for example `start-token.json`) for audit.

There is no DB-scoped control fence. Operators MUST NOT run two control
processes against the same database path concurrently. Loss of a worker
during measurement fails the run; terminals are not reassigned.

Orchestrated remotes (schema, loader, indexes, worker, check, and cleanup
`clean`) follow the process contract in §9.1.

## 8. Results

### 8.1. Per-worker output

Each worker writes `result.json` with:

- `run_id` and instance name;
- its warehouse ranges;
- actual phase timestamps;
- counters, retries, histograms, async-queue telemetry;
- adapter/server version and fatal errors if any.

Workers do not compute final percentiles.

### 8.2. Aggregate

`mind-tpcc consolidate` builds `aggregate.json` as the canonical result. It
MUST embed the concrete run settings (a copy of the effective run-config, or
an equivalent inline object), not merely hashes that point at external config
files.

Consolidation:

1. require the expected worker set for this `run_id`;
2. require complete, non-overlapping warehouse coverage;
3. require complete measurement payloads on each worker (`exit_status`,
   `counters`, `histograms`) and their counter/histogram invariants
   before merge — missing or null fields, negative counters, a non-zero
   completed count (`*_ok + *_user_aborted`) without a response-time
   histogram, missing histogram `min_recorded` / `max_recorded` /
   `sum_values`, and `histogram.total_count` that does not equal that
   completed count are errors, not zeros;
4. merge counters and histogram buckets (including min/max/sum);
5. compute min/max/avg, percentiles and throughput only after the merge;
6. attach check results and a short infrastructure status
   (workers present, assignment OK, clocks OK, no integrity errors,
   TPC-C settings conformant flag and deviation list, …);
7. keep raw per-worker files beside the aggregate for detail.

`mind-tpcc consolidate` MUST also print a brief human summary of the
aggregate (status flags, New-Order throughput, and response-time min/max/avg
and percentiles) to the progress log. The same text is written to
`summary.txt`.

Do not average p99s, scale partial runs, invent zero samples, or emit an
official TPC-C conformance verdict. Soft launch-parameter deviation reporting
is not such a verdict.

Layout:

```text
results/<run_id>/
├── aggregate.json          # settings + merged metrics + status
├── summary.txt             # human view of aggregate.json
├── checks/
├── raw/loader/<instance>/
├── raw/worker/<instance>/
└── orchestrator/
    ├── profile.redacted.yaml
    ├── run-config.json
    ├── start-token.json
    ├── run-state.json
    └── orchestrator.log      # optional; e.g. TPC-C settings warnings
```

Hashes MAY be used operationally (for example to detect a truncated download
of a raw file). They are not the way results refer to configuration: the
aggregate carries the settings themselves.

## 9. Orchestrator Commands

```text
mind-tpcc validate | plan | deploy | undeploy --yes | schema | load | indexes
mind-tpcc check [--after-import|--after-test]
mind-tpcc test | status | stop | collect | consolidate
mind-tpcc run | cleanup --yes
```

`test` arms workers and runs ramp-up / measurement / drain. `start` is a
compatibility alias for `test`. `--skip start` skips the same `run` step.

Standalone `mind-tpcc consolidate` MUST run `collect` first when
`results/<run_id>/collection-manifest.json` is absent, so a post-test
`consolidate` is sufficient to produce `aggregate.json`. `collect` remains
available to re-pull artifacts (for example after a late `check --after-test`).
`run` still executes collect and consolidate as separate pipeline steps;
`--skip collect` still skips only the collect step of `run` and does not
trigger this implicit collect.

`run` = validate → require prior `deploy` (shared worker binaries present on
every assigned host; no auto-upload) → schema → load → indexes →
check(after-import) → test → check(after-test) → collect → consolidate.

`cleanup --yes` tears down an existing run for the profile (explicit
`--run-id`, else the newest matching run, including terminal states). Phases
depend on run-state: stop any recorded running processes; when state is past
`deploying`, launch orchestrated `clean` on the first loader host; when state
is past `planned`, remove `remote_root/<run_id>` on every runtime host; always
remove local `result_root/<run_id>` and `state/runs/<run_id>`. Shared worker
binaries under `remote_root` are left in place.

`undeploy --yes` is the inverse of `deploy`: profile-scoped removal of the
shared worker binary from every assigned host (and loopback deploy-manifest
paths when present). It does not tear down a run; use `cleanup` for that.

Worker artifact semantics are not independently version-negotiated. The
operator **MUST** invoke `deploy` after selecting, building, or updating the
portable-tpcc version and before `run` or any role launch that needs the
worker binary. `deploy` is profile-scoped (shared binary under
`paths.remote_root`; not tied to a `run_id` FSM stage). It **MUST** install on
every assigned host the current set of binary artifacts from that selected
version and **MUST NOT** intentionally reuse binaries from an earlier version.
`run` **MUST NOT** silently re-upload binaries: it only verifies that
`deploy` already placed them, so the operator controls which version is live.
All workers in one run are therefore assumed to execute a homogeneous artifact
set. Mixed worker versions are unsupported and are an operator/deployment
error, not a compatibility mode that `consolidate` is required to reconcile.

Skipped steps are recorded in the run-state and aggregate.

### 9.1. Remote process contract

`mind-tpcc` launches each orchestrated role as a detached process on a runtime
host (`nohup` or equivalent) under
`{paths.remote_root}/{run_id}/{role}/{instance}/`. Re-running a role for the
same `run_id` reuses that instance directory.

Before a new launch, `mind-tpcc` MUST discard leftover `process.json`,
`ready.json`, `result.json`, and `artifact-manifest.json` from that directory
so a previous attempt cannot be adopted as this process. A leftover
`checks/{phase}.json` MUST NOT be treated as this launch unless `process.json`
for the new nonce was observed.

Each launched binary MUST write `process.json` in the instance directory
**immediately after start**, before connecting to the database or doing other
role work. The file MUST include at least:

- OS pid of the binary;
- `instance_nonce` unique to this launch;
- `run_id`, instance name, and role.

The orchestrator uses `process.json` to bind supervision (stop/signal) to this
launch and to reject stale artifacts. It waits only briefly for the file.
Any role that can run longer than that wait (load, indexes, check, workers)
MUST still emit `process.json` first; otherwise the launch is reported as a
metadata timeout while the process is still running.

After role work, the binary MUST write `artifact-manifest.json` in the same
directory with the same `instance_nonce`, `finalized: true`, and
`exit_status`. `mind-tpcc` MUST accept a finalized manifest only when its
nonce matches the launched `process.json`. A non-zero `exit_status` fails the
stage.

`stdout.log` and `stderr.log` in the instance directory are the process
stdio. On check failure, `mind-tpcc` SHOULD also print failed/error items from
the check report (§9.2), not only `exited with status N`.

`mind-tpcc --threads` is a launch-time override for this invocation. It MUST
NOT rewrite an already materialized `run-config.json`. When the flag is set,
`mind-tpcc` passes `--threads=N` on orchestrated `loader`, `worker`, and
`check` argv. Every `tpcc-<dbms>` binary MUST accept the same forms
(`--threads=N`, `--threads N`, `-t N`):

- `worker`: coroutine threads. `0` means auto at the binary (same as
  standalone `run` / `ComputeRunLayout`). When the flag is omitted, the worker
  uses `worker_assignment[].threads` from run-config.
- `loader`: import concurrency. `0` means auto. When omitted, the loader uses
  `load_assignment[].threads`.
- `check`: parallel DBMS sessions; see §9.2. `mind-tpcc` always passes a
  resolved positive N.

### 9.2. Integrity-check reports

Orchestrated `check` uses instance `check-0` on the first loader host.
`--after-import` and `--after-test` are separate invocations.

`mind-tpcc` passes `--threads=N` from CLI `--threads` when that flag is set,
otherwise from `runtime.check_concurrency` (0 / omit =
`min(scale.warehouses, 32)`). A missing or non-positive value on the binary
means one session; `mind-tpcc` MUST pass a resolved positive N. Adapters MUST honor
`TCheckRequest.CheckConcurrency` as parallel DBMS sessions. When that value is
greater than one, each parallel worker MUST open its own DBMS session.
Catalog ids run one after another in catalog order; parallel sessions apply
to warehouse chunks of the catalog id currently running. Mixing several
catalog ids in one work-stealing queue delays the stdout progress line until
the whole suite finishes and MUST NOT be used.

Warehouse-scoped scans use inclusive `w_id` chunks of size 1
(`kWarehouseCheckRange`) so HASH partition pruning (OceanBase) and
per-warehouse parallelism apply on every adapter. Which catalog identifiers
are warehouse-ranged MUST be the same on every adapter. Adapters MUST NOT use
a private chunk size.

SQL predicates stay those of TPC-C §3.3.2; only scheduling and warehouse
filter bounds change. A change to check scheduling (concurrency wiring, chunk
size, or which catalog ids are warehouse-ranged) MUST be applied to every
adapter. Do not leave one DBMS serial or on a private range size. Do not
rewrite §3.3.2 predicates for speed (PX / `PARALLEL` hints, skipped catalog
entries, DBMS-only SQL “optimizations”) unless explicitly requested. Dialect
translation that keeps the same condition (for example OceanBase `LEFT JOIN` +
`UNION ALL` instead of `FULL JOIN`) is allowed.

Stdout `Checking … [OK]/[Failed]/[Skipped]` is informative: one line per
catalog id after that job completes (all warehouse chunks). Per-chunk progress
lines are not required. The JSON report is the structured contract for
orchestrator diagnostics and consolidate.

The check role MUST write `{run_dir}/checks/{phase}.json` on the runtime host
before the artifact manifest (`phase` is `after-import` or `after-test`).
`collect` copies those files to `results/<run_id>/checks/`.

The report MUST be JSON with at least:

| Field | Meaning |
| --- | --- |
| `ok` | `true` iff `failed == 0` and `errors == 0` |
| `phase` | `after-import` or `after-test` |
| `passed`, `failed`, `skipped`, `errors` | counts |
| `checks[]` | entries with `id`, `title`, `status`, `detail` |
| `checks[].status` | `passed`, `failed`, `skipped`, or `error` |

If the adapter exposes a query/statement timeout in the profile or connection
string, **every check session MUST use it**, including each parallel worker
session. Worker OLTP sessions MAY keep the DBMS default so a hung transaction
fails fast. OceanBase:
`database.options.query_timeout` (seconds, default 600) sets session
`ob_query_timeout` for load, indexes, statistics, and check; the server
default without that SET is 10s. See [run-oceanbase.md](run-oceanbase.md).

Secrets: the profile names a control-host environment variable
(`password_env`); `mind-tpcc` delivers the value to workers as a mode-0600
`password_file` beside `run-config.json` and must not place the secret in
argv, SSH/nohup command lines, profile artifacts, or logs. Host-key checking
is required unless explicitly disabled in the profile (recorded in run-state).

## 10. Validation

Reject unknown fields/DBMS, bad instance names, empty instance lists, more
instances than warehouses, manual assignment fields in the profile, invalid
mix, non-positive sizes/timeouts, secret literals, and retry-after-ambiguous
commit. `runtime.histogram.unit`, if present, MUST be `ms` or `us`.
`runtime.histogram.highest`, if present, MUST be greater than zero (omitted
uses the built-in default rather than silently substituting it for `<= 0`).

Additionally, compare effective (default-merged) launch parameters against the
fixed TPC-C 5.11 requirements used by built-in defaults: terminals per
warehouse = 10; Clause 5.2.3 mix minima (Payment ≥ 43%, Order-Status /
Delivery / Stock-Level ≥ 4%; New-Order has no mix minimum); pacing enabled;
exponential think time; keying times and mean think times at least the
Clause 5.2.5.7 minima (larger values remain conformant); and measurement
interval ≥ 120 minutes. Report deviations in `mind-tpcc validate`, warn at
`test`, and persist `tpcc_settings_conformant` plus
`tpcc_settings_deviations` in the aggregate. These deviations MUST NOT fail
structural validation or change `result_class`. Phase durations MUST NOT be
negative; `phases.measurement` MUST be greater than zero (a shorter-than-120m
positive interval is a soft TPC-C deviation, not a structural error).

## 11. Physical Schema Notes

Adapters map the shared logical schema to DDL. They MAY add partitions,
indexes, and technical keys if logical semantics stay intact and the choice is
visible in the result settings/options.

- **YDB:** warehouse-leading keys, range partitions, typed bulk load; no
  exact values as `Double`; no hidden SDK retries; prefer
  `ExecuteFinalAndCommit` for the last statement as an async fused pipeline
  (no `GetValueSync()` on the task-queue thread; adapter-api §4.3).
- **PostgreSQL:** prepared statements, `COPY`, DECIMAL, SQLSTATE mapping,
  blocking libpqxx IO on a bounded `IExecutor` in `PgSession` (not inside
  `ITpccTransaction` on the scheduler), `ANALYZE` after indexes. Optional
  warehouse `HASH` partitions (see
  [pgsql-partitioning-design.md](pgsql-partitioning-design.md)).
- **OceanBase:** warehouse partitioning, cached statements, clear error
  classes, optional FKs as a recorded physical option, optional
  `CREATE INDEX … PARALLEL n` (`database.options.index_parallel`, default 4),
  `DBMS_STATS.GATHER_TABLE_STATS` with gather DOP equal to the HASH partition
  count. Session `ob_query_timeout` (`database.options.query_timeout`, default
  600s) MUST apply to load, indexes, statistics, and integrity-check sessions
  (specification §9.2); worker OLTP sessions MAY keep the server default (10s).
  Blocking connector IO belongs on a bounded `IExecutor` in `TObSession`;
  `ITpccTransaction` MUST NOT `.Get()` on the scheduler (adapter-api §4.3).

See [adapter-api.md](adapter-api.md) §5–§6 for the full logical/physical and
query-binding contract.

## 12. Repository Layout

```text
tpcc/
├── domain/ generator/ transactions/ runtime/ loader/ checks/ metrics/
├── dbms/{ydb,pgsql,oceanbase}/
└── app/{ydb,pgsql,oceanbase}/
mind/
docs/specification.md
docs/adapter-api.md
docs/async-adapter-transactions.md
docs/examples/
```

Build with existing `ya make` (C++). Use Go-native tools for Golang. No alternate root build system.

## 13. Done When

- Same seed → equivalent logical data on all adapters.
- Multi-host workers cover warehouses without duplicate home terminals.
- Phases sync; warmup excluded from measurement.
- Load is safely retryable; post-import checks pass.
- Worker DBMS IO does not block scheduler threads; observed `Inflight` can
  exceed `ThreadCount` up to `max_inflight` (adapter-api §4.3).
- Aggregate embeds concrete settings and reproduces merged metrics from raw
  worker files without DBMS access.
- No secrets in stored configs/results.

## 14. Open Decisions

1. ~~C++ coroutine/future ABI for shared libraries.~~ **Resolved:** shared
   libraries use `TFuture` (see [adapter-api.md](adapter-api.md) §4.3).
2. ~~Histogram bucket layout and max latency.~~ **Resolved for engineering
   artifacts:** `linear_exp` with profile knobs `unit` + `highest`; worker
   derives `hdr_till` (default 4096, capped by `highest`) and publishes
   effective `{unit, highest, layout, hdr_till, max_value}`. HDR-style
   `lowest` / `significant_figures` are rejected.
3. Per-DBMS ambiguous-commit handling.
4. Canonical row bytes for cross-DB sample checks.
5. Minimum supported YDB / PostgreSQL / OceanBase versions.

Further alignment sequencing: [alignment-plan.md](alignment-plan.md).
Worker `ITpccTransaction` async contract:
[async-adapter-transactions.md](async-adapter-transactions.md).
