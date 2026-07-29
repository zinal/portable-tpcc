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
not judge whether parameters match any TPC-C edition.

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
- Continuing measurement after a worker is lost.
- Dynamic terminal rebalancing during measurement.
- Multi-edition TPC-C support or conformance-to-edition checks.
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

Generator streams are keyed by
`(run_seed, purpose, warehouse, district, terminal, sequence)`. Parallelism
MUST NOT change logical contents for a given seed.

### 4.2. Adapter API

Each `tpcc/dbms/<name>` implements admin, load (`PutBatch`), session/factory,
checks, error classification, and capabilities. SDK types stay inside the
adapter.

Session surface:

```text
Begin / Execute / ExecuteBatch / ExecuteFinalAndCommit
Commit / Rollback / Cancel
```

### 4.3. Binaries

`tpccctl` orchestrates. Each `tpcc-<dbms>` binary exposes roles such as
`schema`, `loader`, `worker`, and `check`. Non-DBMS logic comes from shared
libraries; only the adapter/driver is DBMS-specific.

## 5. Configuration Model

Human-edited input: one YAML profile (`portable-tpcc/v1`).

`tpccctl` validates the profile, fills built-in defaults for omitted workload
fields, computes warehouse assignment, and writes a normalized
`run-config.json`. That file is the only declarative input distributed to
loaders and workers.

`run-config.json` includes concrete values, not hash stand-ins for other
documents:

- `run_id`, DBMS settings (no passwords — only `password_env`);
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

Phases: prepare → ramp-up → measurement → drain. Phase schedule comes from
the run-config. Warmup samples do not enter measurement metrics. Async work
uses a bounded drain window from the run-config.

Startup (simple barrier):

1. Distribute the same `run-config.json` to all hosts.
2. Each worker prepares and reports ready (instance name, ranges, clock skew
   sample).
3. When the expected set is ready, `tpccctl` writes `start-token.json` with
   absolute phase timestamps and starts the run.
4. Workers admit work only after accepting that token.

A DB-scoped fence (adapter metadata outside benchmark tables) prevents two
control processes from driving the same database path concurrently. Loss of a
worker during measurement fails the run; terminals are not reassigned.

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
3. merge counters and histogram buckets;
4. compute percentiles and throughput only after the merge;
5. attach check results and a short infrastructure status
   (workers present, assignment OK, clocks OK, no integrity errors, …);
6. keep raw per-worker files beside the aggregate for detail.

Do not average p99s, scale partial runs, invent zero samples, or emit a TPC-C
conformance verdict.

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
    └── run-state.json
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

Skipped steps are recorded in the run-state and aggregate.

Secrets: passwords only via environment variable names; never in profile
artifacts, argv, run-config, or logs. Host-key checking is required unless
explicitly disabled in the profile (recorded in run-state).

## 10. Validation (structural only)

Reject unknown fields/DBMS, bad instance names, empty instance lists, more
instances than warehouses, manual assignment fields in the profile, invalid
mix, non-positive sizes/timeouts, secret literals, and retry-after-ambiguous
commit. Do not reject profiles for differing from a TPC-C edition.

## 11. Physical Schema Notes

Adapters map the shared logical schema to DDL. They MAY add partitions,
indexes, and technical keys if logical semantics stay intact and the choice is
visible in the result settings/options.

- **YDB:** warehouse-leading keys, range partitions, typed bulk load; no
  exact values as `Double`; no hidden SDK retries.
- **PostgreSQL:** prepared statements, `COPY`, DECIMAL, SQLSTATE mapping,
  bounded IO if using blocking libpqxx, `ANALYZE` after indexes.
- **OceanBase:** warehouse partitioning, cached statements, clear error
  classes, optional FKs as a recorded physical option.

## 12. Repository Layout

```text
tpcc/
├── domain/ generator/ transactions/ runtime/ loader/ checks/ metrics/
├── dbms/{ydb,pgsql,oceanbase}/
└── app/{ydb,pgsql,oceanbase}/
tools/tpccctl/
docs/specification.md
docs/examples/
```

Build with existing `ya make` (C++ and Go). No alternate root build system.

## 13. Done When

- Same seed → equivalent logical data on all adapters.
- Multi-host workers cover warehouses without duplicate home terminals.
- Phases sync; warmup excluded from measurement.
- Load is safely retryable; post-import checks pass.
- Aggregate embeds concrete settings and reproduces merged metrics from raw
  worker files without DBMS access.
- No secrets in stored configs/results.

## 14. Open Decisions

1. C++ coroutine/future ABI for shared libraries.
2. Histogram bucket layout and max latency.
3. Per-DBMS ambiguous-commit handling.
4. Canonical row bytes for cross-DB sample checks.
5. Minimum supported YDB / PostgreSQL / OceanBase versions.
