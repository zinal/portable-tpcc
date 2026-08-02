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
3. the Go orchestrator `tpccctl`.

Shipped binaries: `tpccctl`, `tpcc-ydb`, `tpcc-pgsql`, `tpcc-oceanbase`.
There is no extra DBMS-neutral helper binary.

Initial adapters: YDB, PostgreSQL, OceanBase. Adding a DBMS MUST NOT require
forking the shared workload logic.

This document does not restate the TPC-C standard. The project implements one
fixed workload model. Run parameters (transaction mix, think/keying times,
terminals per warehouse, and similar) are defaults in `tpccctl` and MAY be
overridden in the profile. The tool does not select TPC-C editions and does
not emit an official TPC-C conformance verdict. It DOES report soft deviations
of effective launch parameters from the fixed TPC-C 5.11 requirements used by
the built-in defaults (validate output, start warning, aggregate status).

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
          tpccctl (control host)
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
| control | 1 | `tpccctl`: plan, deploy, drive phases, collect, merge |
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
[adapter-api.md](adapter-api.md)):

```text
Begin / Execute / ExecuteBatch / ExecuteFinalAndCommit
Commit / Rollback / Cancel
```

Semantic operations are a closed set of tagged structs (`std::variant` or
equivalent), not opaque `void*` blobs.

Details, module boundaries, PostgreSQL reference mapping, and DBMS-specific
guidance (YDB fused commit, DDL/query differences, OceanBase notes) are in
[adapter-api.md](adapter-api.md).

### 4.3. Binaries

`tpccctl` orchestrates. Each `tpcc-<dbms>` binary **MUST** expose the
normative roles `schema`, `loader`, `worker`, and `check`. Non-DBMS logic
comes from shared libraries; only the adapter/driver is DBMS-specific.

Binaries **MAY** keep standalone aliases for local use (`init` ≡ `schema`,
`import` / `run` with flag-driven config, `clean` as local-only admin).
Orchestrated remotes use only the four normative role names.

## 5. Configuration Model

Human-edited input: one YAML profile (`portable-tpcc/v1`).

`tpccctl` validates the profile, fills built-in defaults for omitted workload
fields, computes warehouse assignment, and writes a normalized
`run-config.json`. That file is the only declarative input distributed to
loaders and workers.

`run-config.json` includes concrete values, not hash stand-ins for other
documents:

- `run_id`, DBMS settings (no passwords or tokens — only `password_env`);
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

Examples: [profile.v1.yaml](examples/profile.v1.yaml),
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
2. `tpccctl` chooses an absolute UTC start instant
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
   missed deadline), `tpccctl` MUST abort the run and stop remaining
   processes.
7. At `--start-at`, workers begin ramp-up; later phase boundaries follow
   run-config durations. The orchestrator SHOULD record the chosen schedule
   under `orchestrator/` (for example `start-token.json`) for audit.

There is no DB-scoped control fence. Operators MUST NOT run two control
processes against the same database path concurrently. Loss of a worker
during measurement fails the run; terminals are not reassigned.

Process supervision may use `nohup` plus an instance lock and `process.json`
(PID, start time, nonce) so stop/restart does not signal the wrong process.

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

`tpccctl consolidate` builds `aggregate.json` as the canonical result. It
MUST embed the concrete run settings (a copy of the effective run-config, or
an equivalent inline object), not merely hashes that point at external config
files.

Consolidation:

1. require the expected worker set for this `run_id`;
2. require complete, non-overlapping warehouse coverage;
3. merge counters and histogram buckets (including min/max/sum);
4. compute min/max/avg, percentiles and throughput only after the merge;
5. attach check results and a short infrastructure status
   (workers present, assignment OK, clocks OK, no integrity errors,
   TPC-C settings conformant flag and deviation list, …);
6. keep raw per-worker files beside the aggregate for detail.

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
tpccctl validate | plan | deploy | schema | load
tpccctl check [--after-import|--after-run]
tpccctl start | status | stop | collect | consolidate
tpccctl run | cleanup --yes
```

`run` = validate → deploy → schema → load → check(after-import) → start
phases → check(after-run) → collect → consolidate.

Worker artifact semantics are not independently version-negotiated. The
operator **MUST** invoke `deploy` after selecting, building, or updating the
portable-tpcc version and before starting a run. `deploy` **MUST** install on
every assigned host the current set of binary artifacts from that selected
version and **MUST NOT** intentionally reuse binaries from an earlier version.
All workers in one run are therefore assumed to execute a homogeneous artifact
set. Mixed worker versions are unsupported and are an operator/deployment
error, not a compatibility mode that `consolidate` is required to reconcile.

Skipped steps are recorded in the run-state and aggregate.

Secrets: passwords only via environment variable names; never in profile
artifacts, argv, run-config, or logs. Host-key checking is required unless
explicitly disabled in the profile (recorded in run-state).

## 10. Validation

Reject unknown fields/DBMS, bad instance names, empty instance lists, more
instances than warehouses, manual assignment fields in the profile, invalid
mix, non-positive sizes/timeouts, secret literals, and retry-after-ambiguous
commit.

Additionally, compare effective (default-merged) launch parameters against the
fixed TPC-C 5.11 requirements mirrored by built-in defaults: terminals per
warehouse, minimum mix percentages, pacing enabled, exponential think time,
standard keying/think means, and measurement interval ≥ 120 minutes. Report
deviations in `tpccctl validate`, warn at `start`, and persist
`tpcc_settings_conformant` plus `tpcc_settings_deviations` in the aggregate.
These deviations MUST NOT fail structural validation or change
`result_class`.

## 11. Physical Schema Notes

Adapters map the shared logical schema to DDL. They MAY add partitions,
indexes, and technical keys if logical semantics stay intact and the choice is
visible in the result settings/options.

- **YDB:** warehouse-leading keys, range partitions, typed bulk load; no
  exact values as `Double`; no hidden SDK retries; prefer
  `ExecuteFinalAndCommit` for the last statement.
- **PostgreSQL:** prepared statements, `COPY`, DECIMAL, SQLSTATE mapping,
  bounded IO if using blocking libpqxx, `ANALYZE` after indexes.
- **OceanBase:** warehouse partitioning, cached statements, clear error
  classes, optional FKs as a recorded physical option.

See [adapter-api.md](adapter-api.md) §5–§6 for the full logical/physical and
query-binding contract.

## 12. Repository Layout

```text
tpcc/
├── domain/ generator/ transactions/ runtime/ loader/ checks/ metrics/
├── dbms/{ydb,pgsql,oceanbase}/
└── app/{ydb,pgsql,oceanbase}/
tpccctl/
docs/specification.md
docs/adapter-api.md
docs/examples/
```

Build with existing `ya make` (C++). Use Go-native tools for Golang. No alternate root build system.

## 13. Done When

- Same seed → equivalent logical data on all adapters.
- Multi-host workers cover warehouses without duplicate home terminals.
- Phases sync; warmup excluded from measurement.
- Load is safely retryable; post-import checks pass.
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
