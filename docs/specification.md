# portable-tpcc Specification

Status: architecture draft, version 1.

The keywords **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY**
are to be interpreted as described in RFC 2119.

## 1. Purpose

`portable-tpcc` is a horizontally scalable TPC-C workload generator for
multiple DBMSs. The project consists of:

1. shared C++ libraries containing the TPC-C model, data generator, terminal
   runtime, metrics, and checks;
2. DBMS adapters;
3. executables linked with a specific adapter;
4. a separate `tpccctl` orchestrator that prepares the database, distributes
   the workload across hosts, synchronizes phases, collects artifacts, and
   produces a consolidated result.

The initial set of adapters:

- YDB;
- PostgreSQL;
- OceanBase.

The architecture must not require a fork of the shared logic to add a DBMS.

This document neither replaces nor restates the TPC-C standard. `portable-tpcc`
implements a single fixed TPC-C-style workload model (schema shape, generator,
and transaction logic). Workload parameters that govern a run — including the
transaction mix, think/keying times, terminal counts, and related defaults —
are owned by the `tpccctl` orchestrator: they are embedded as code defaults and
MAY be customized through the orchestrator profile. The project does not
provide a framework for selecting, packaging, or evolving multiple TPC-C
editions, and it does not validate run parameters against any edition of the
TPC-C standard.

## 2. Scope

### 2.1. Goals

- One logical TPC-C database and an arbitrary number of generator hosts.
- Unambiguous ownership of warehouses by worker processes.
- Horizontally scaled initial data loading.
- Identical data and logical transaction inputs for all adapters.
- DBMS-specific DDL, bulk load, SQL, retry mapping, and physical layout.
- Synchronized ramp-up, measurement, and drain boundaries.
- Merging of counters and histograms without averaging percentiles.
- A complete, reproducible run manifest.
- Automated integrity and infrastructure checks before and after the test.

### 2.2. Non-goals for the Initial Version

- Provisioning or administration of DBMS clusters.
- Requiring Kubernetes, Ansible, or systemd as the environment.
- A single universal SQL dialect.
- Support for server-side stored procedures as a portable interface.
- Automatic TPC result certification.
- Automatic continuation of measurement after a worker is lost.
- Dynamic terminal rebalancing during measurement.
- Support for multiple, selectable, or evolving TPC-C editions.
- Checking that configured parameters (mix, timings, scale constraints, and
  similar) match those prescribed by any TPC-C edition.
- Producing a conformance or qualification verdict against the TPC-C standard.

`portable-tpcc` MUST call a result an official TPC-C result only after
independent verification of all TPC requirements. By default, the report
contains `result_class: engineering`.

## 3. Overall Architecture

```text
                         CONTROL HOST
                ┌─────────────────────────┐
                │ tpccctl                 │
                │ profile / state / merge │
                └──────┬─────────────┬────┘
                  SSH  │             │ artifacts
          ┌────────────┴──────┐  ┌───┴────────────┐
          │ loader processes  │  │ worker processes│
          │ global + W ranges │  │ W ranges        │
          └───────────┬───────┘  └───────┬────────┘
                      │                  │
                      └────────┬─────────┘
                               │ adapter API
                    ┌──────────▼──────────┐
                    │ one logical database│
                    │ YDB / PG / OB       │
                    └─────────────────────┘
```

Roles:

| Role | Count | Responsibility |
| --- | ---: | --- |
| `control` | 1 | `tpccctl`, the sole mutable run-state |
| `db` | 1 logical database | a pre-provisioned and accessible DBMS |
| `loader` | 1..N | deterministic, non-overlapping portions of the load |
| `worker` | 1..N | workload for assigned warehouses |
| `results` | 1 | consolidated artifact directory |

## 4. Components and Library Boundaries

### 4.1. `tpcc/spec`

A single C++ package provides the fixed workload model used by generators,
runtime binaries, and contract tests: schema AST helpers, scale/seed
materialization, terminal derivation, expected cardinalities, and related test
vectors. There is no edition selector and no package tree of standard
revisions.

The package is built as:

- a library linked by `domain`, workload binaries, and contract tests;
- the DBMS-neutral `tpcc-spec` CLI used by the Go orchestrator for pure
  materialization helpers;
- JSON Schema for CLI inputs and outputs.

Minimum CLI:

```text
tpcc-spec describe
tpcc-spec materialize --scale <json> --seed-source <json>
tpcc-spec derive-terminals --spec-state <json> --assignment <json>
tpcc-spec expected-data --spec-state <json> --load-plan <json>
```

All commands are pure functions and output canonical JSON. `describe` returns
the module SHA. `materialize` creates an opaque `spec-state.json` for the
generator; the orchestrator does not interpret its internal fields.

The orchestrator owns run parameters (transaction mix, think/keying times, and
similar defaults). It materializes them into `run-config.json` and does not ask
`tpcc-spec` to validate those parameters against any TPC-C edition. At
startup, the workload binary verifies that the linked module SHA matches
`spec-state.json`.

### 4.2. `tpcc/domain`

Independent of DBMS SDKs, networking, and the scheduler:

- identifier types;
- exact numeric types;
- immutable transaction input and output types;
- NURand and string generation;
- derivation of deterministic RNG streams;
- initial population rules;
- shared business calculations;
- invariants and expected cardinalities.

In C++, exact decimal values are represented by checked fixed-point types;
conversion to `double` within the domain and adapter API MUST NOT be used.

### 4.3. `tpcc/generator`

- creates shared generator parameters for a test run;
- creates initial data;
- creates logical transaction input;
- provides independent streams keyed by:
  `(run_seed, purpose, warehouse, district, terminal, sequence)`;
- generates identical values regardless of the number of loader/worker
  processes.

Parallelism MUST NOT change the database contents. For a given `run_seed`, the
hash of the canonical form of each record is identical under any sharding.

### 4.4. `tpcc/transactions`

Contains the shared sequence of business operations. The library operates
through the typed `ITpccSession`, rather than through SQL, `pqxx`, the YDB SDK,
or `MYSQL*`.

Normative adapter API:

```text
Begin(TIsolation) -> TTransaction
Execute(TTransaction&, TSemanticOperation) -> TOperationResult
ExecuteBatch(TTransaction&, TSemanticBatch) -> TBatchResult
ExecuteFinalAndCommit(TTransaction&, TSemanticOperation)
    -> {TOperationResult, TCommitOutcome}
Commit(TTransaction&) -> TCommitOutcome
Rollback(TTransaction&) -> TRollbackOutcome
Cancel(TTransaction&) -> TCancelOutcome
```

`TTransaction` has the explicit states `active`, `committing`, `committed`,
`rolled_back`, and `outcome_unknown`. `TCommitOutcome` contains certainty and
native diagnostics. An operation result specifies the expected cardinality;
violating that expectation is an `integrity` error, not an empty successful
result.

The batch boundary allows YDB to execute set-oriented YQL, PostgreSQL to use
prepared SQL/COPY, and OceanBase to use cached prepared statements without
changing the shared algorithm. `ExecuteFinalAndCommit` allows YDB to combine
the final query and commit without hidden deferred side effects. Additional
fusion is permitted only within a single semantic operation/batch.

### 4.5. `tpcc/runtime`

- terminal state machines from the assignment in the run-config;
- coroutine scheduler;
- keying and think time;
- admission control;
- immutable logical transaction envelope;
- retry loop;
- executor for asynchronous parts of the workload;
- phase barriers;
- mergeable metrics;
- graceful drain.

The runtime depends only on `domain`, `transactions`, and the abstract adapter
API.

### 4.6. `tpcc/loader`

- builds a row plan per shard;
- designates the sole owner of the DB-wide dataset;
- creates a deterministic batch with a stable identifier and hash;
- passes it to the single idempotent `PutBatch`;
- MAY maintain a local cache of successfully completed batches solely as an
  optimization;
- verifies cardinalities and canonical sample hashes after loading.

### 4.7. `tpcc/checks`

Shared definitions:

- a catalog of integrity and infrastructure checks;
- an identifier and expected result type for each check;
- checks for load completeness and shard consistency;
- infrastructure checks for phases, ownership, and artifacts.

The SQL/query for each condition is implemented by the adapter, but the
identifier, expected semantics, and result format are shared. The catalog does
not include checks that the configured mix, timings, or other run parameters
match any TPC-C edition.

### 4.8. `tpcc/metrics`

- counters;
- histograms with shared boundaries;
- error/retry events;
- worker result serialization;
- deterministic merging;
- infrastructure health flags.

### 4.9. Adapters

Each `tpcc/dbms/<name>` implements:

1. `IAdminAdapter` — schema, index, analyze/compact, clean, and metadata;
2. `ILoadAdapter` — idempotent `PutBatch` and `Ensure*` operations;
3. `ISessionFactory` / `ITpccSession` — transactions;
4. `ICheckAdapter` — queries for shared invariants;
5. `IErrorClassifier` — normalized errors;
6. `ICapabilities` — isolation, batch, commit, cancellation, and topology;
7. DBMS-specific configuration and its strict validation.

SDK types MUST NOT cross the adapter boundary.

### 4.10. Executables

The initial version builds separate programs:

```text
tpcc-ydb
tpcc-pgsql
tpcc-oceanbase
tpcc-spec
tpccctl
```

C++ programs link the same shared libraries and one adapter. This approach
does not require a runtime plugin ABI and does not pull the client libraries
for every DBMS into a single binary.

`tpccctl` is a separate self-contained Go binary.

## 5. Horizontally Scaled Workload Execution

### 5.1. Assignment

The orchestrator and `tpcc-spec` construct the full set of terminal identities
for the specified scale using the terminals-per-warehouse setting from the
run-config. The user profile lists only loader/worker instances and hosts; the
profile contains neither manual warehouse ranges nor a DB-wide data ownership
flag.

The assignment defines ownership of a warehouse's **home terminals**; it does
not restrict transactions from accessing rows belonging to other warehouses.
A warehouse's set of home terminals MUST NOT be split across worker processes.

`tpccctl` applies `balanced-contiguous-v1` separately to loaders and workers:

1. sorts instances by ASCII name in bytewise ascending order;
2. divides the number of warehouses by the number of instances;
3. assigns one additional warehouse to the first
   `warehouse_count % instance_count` instances;
4. constructs contiguous half-open ranges without overlaps or gaps;
5. assigns DB-wide data to the first loader in the same order.

The number of instances MUST be positive and must not exceed the number of
warehouses. The algorithm, input instance set, and computed assignment are
recorded in `run-config.json`/`load-plan.json` and displayed by the `plan`
command before any side effects. v1 provides no manual assignment override.

Adding a worker changes only the assignment. It MUST NOT change the logical
scale, the number or identity of terminals, or the generator parameters. The
worker receives its assignment in the immutable `run-config.json` and verifies
its SHA-256 from the start token.

Static ownership is intentional: dynamic reassignment during measurement
changes pacing, RNG streams, and the connection set, and is therefore
prohibited.

### 5.2. Worker Runtime

A worker contains:

- terminal state machines for its assigned warehouses;
- a coroutine scheduler and monotonic timers;
- a concurrency limiter for adapter calls;
- separate executors for asynchronous parts of the workload;
- local counters and mergeable histograms;
- a phase controller and graceful drain.

Each terminal state is serial: the next logical operation does not begin until
the preceding operation reaches a terminal state. The number of OS threads,
connections, and inflight operations are worker performance parameters, not
logical scale parameters.

### 5.3. Logical Transaction and Retry

Before the first attempt, the runtime creates an immutable envelope:

```json
{
  "run_id": "...",
  "worker_id": "...",
  "terminal_id": "...",
  "sequence": 42,
  "transaction": "...",
  "input": {},
  "input_timestamp": "...",
  "logical_id": "..."
}
```

All inputs, timestamps, and the logical ID MUST remain unchanged until the
final outcome. The shared runtime does not allow adapters to invoke the
generator again during a retry.

Normalized error classes:

| Class | Action |
| --- | --- |
| `retryable_abort` | rollback confirmed; bounded retry with backoff+jitter |
| `not_committed` | safe to retry according to the adapter contract |
| `ambiguous_commit` | MUST NOT retry without backend-specific resolution |
| `permanent` | complete the operation with an error; policy determines the fate of the run |
| `integrity` | fail the run |
| `cancelled` | phase termination, not a retry |

Internal SDK retries and shared runtime retries MUST form a single observable
budget. The number of attempts, delays, and native error codes are recorded in
the metrics.

### 5.4. Asynchronous Parts of the Workload

Deferred operations required by the workload are implemented by the shared
runtime through a typed bounded queue and a separate executor. The adapter
executes only the atomic database portion of a queue item.

The queue records the logical ID and enqueue/start/completion times. At the
measurement boundary, admission stops and already accepted items receive a
separate drain window. Drain limits come from the orchestrator run-config
(defaults embedded in `tpccctl`, overridable in the profile).

## 6. Separation of Logical and Physical Schemas

### 6.1. Shared Schema Model

The spec module describes tables, columns, constraints, and logically required
access paths in a DBMS-neutral AST. It is the sole source of the schema for
the generator, checks, and adapter contract tests.

The adapter transforms the AST into DDL and a physical layout. It MAY add
technical keys, indexes, partitions, and storage options provided that:

- logical visibility and transactional semantics remain unchanged;
- the addition is recorded in the run manifest;
- equivalent indexes are not duplicated;
- shared checks can distinguish logical data from technical data.

Exact decimal domain types MUST map to exact DBMS types.

### 6.2. YDB

The adapter SHOULD:

- place the warehouse key first for warehouse-local tables;
- use range partitioning and document the split policy;
- use `GlobalSync` indexes only where required by queries;
- use typed `BulkUpsert` for loading;
- use set-oriented operations and commit in the final query;
- treat topology hints as recommendations rather than changing the logical
  scale;
- not map exact domain values to `Double`;
- not hide retries inside `RetryQuery`.

`.sys/nodes`, compaction, index implementation tables, and YDB status codes
remain within the adapter.

### 6.3. PostgreSQL

The adapter SHOULD:

- use prepared statements and `COPY`;
- specify fully-qualified identifiers instead of depending on `search_path`;
- map exact domain values to DECIMAL;
- classify errors by SQLSTATE;
- apply sufficient isolation and row locking;
- not create one OS thread per coroutine when non-blocking transport is
  available; if blocking libpqxx is used, the IO pool MUST be bounded;
- create the customer index after bulk load and then run `ANALYZE`.

### 6.4. OceanBase

The adapter SHOULD:

- use tablegroup/hash partitioning by warehouse key;
- configure DB-wide and warehouse-scoped data separately;
- use cached prepared statements and parameter binding;
- distinguish deadlock, lock timeout, serialization failure, killed
  transaction, connection loss, and ambiguous commit;
- validate `max_inflight > 0`;
- support explicit selection of foreign keys as a physical setting recorded in
  the result;
- run `ANALYZE` after creating indexes;
- not treat a MariaDB integration test as a substitute for an OceanBase test.

Connector/C, local index syntax, timeout variables, and catalog queries remain
within the adapter.

## 7. Horizontally Scaled Loading

### 7.1. Plan

The orchestrator constructs `load-plan.json`:

- first, a canonical `plan_payload` consisting only of assignments and batches;
- `plan_payload_sha256` — the hash of the entire plan payload;
- `load_id` — the SHA-256 of the canonical tuple
  `(run_id, plan_payload_sha256, spec-state SHA, loader binary SHA)`;
- the final document contains the payload, `plan_payload_sha256`, and
  `load_id`, after which the hash of the entire `load-plan.json` is computed
  separately;
- exactly one shard owns the DB-wide data defined by the spec module;
- warehouse-scoped data is divided into non-overlapping warehouse ranges;
- a batch has a `batch_id`, key range, row count, and its own
  `batch_payload_sha256` for the canonical rows;
- assignment does not depend on launch order.

### 7.2. Resumption

The loader has a single DBMS-neutral contract:

```text
PutBatch(load_id, batch_id, table, key_range, rows, batch_payload_sha256)
    -> completed | outcome_unknown | failed
```

`PutBatch` MUST be idempotent: any number of retries of the same batch,
including after `outcome_unknown` or a crash, produces the same final set of
rows as one successful execution. An unconfirmed batch is therefore always
retried without a separate recovery mode.

Before the first batch, the adapter atomically binds the workload path to the
`load_id`. An empty path accepts a new identifier; a partially loaded path
accepts only the same identifier. The presence of a different `load_id` is an
`integrity` error and requires an explicit `clean` or a new path; this prevents
mixing two datasets and retaining residual rows from another scale.

To satisfy the contract:

- rows and all their values are fully determined by `spec-state`;
- a batch contains complete values rather than relative increments;
- logical and technical keys are stable;
- server-generated timestamps, sequences, and other changing defaults are
  prohibited during loading;
- `batch_id` is bound to the `load_id`, key range, and
  `batch_payload_sha256`;
- a different payload for the same batch identity is an `integrity` error;
- a table without a suitable logical key receives a deterministic technical
  key or is implemented by the adapter using staging + replace-range.

`PutBatch` is a semantic operation, not a requirement to use SQL `INSERT`. The
adapter MAY use upsert, staging/merge, replace-range, or an internal ledger,
but these alternatives are not visible to the loader or profile.

A local checkpoint of successful batches MAY accelerate a repeated `load`. It
is bound to the run/profile/load-plan/spec-state/binary hashes and is not a
condition of correctness: if the checkpoint is absent or uncertain, the batch
is simply retried.

Schema, index, and statistics creation is performed by the separate idempotent
operations `EnsureSchema`, `EnsureIndexes`, and `EnsureStatistics`. After all
`PutBatch` operations, the orchestrator runs post-import checks; their result
is the authoritative confirmation that the load is complete.

### 7.3. Post-import Checks

The following MUST be performed on a quiescent database:

- all checks marked `after-import` in the shared check catalog;
- batch manifest completeness;
- absence of overlaps and gaps in the load assignment;
- correspondence of actual cardinalities to the expected values computed by
  the spec module for the scale;
- canonical sample hashes that are identical for all adapters;
- readiness of DBMS-specific indexes/statistics.

The report stores check identifiers and machine-readable results.

## 8. The `tpccctl` Orchestrator

### 8.1. Principles

The control plane follows these principles:

1. one self-contained Go binary;
2. a declarative YAML profile, `portable-tpcc/v1`;
3. workload parameters (transaction mix, think/keying times, and related
   defaults) embedded in orchestrator code and overridable via the profile;
4. SSH + SFTP/tar without a mandatory agent;
5. `plan` without side effects;
6. an immutable `run-config.json` that is byte-identical on runtime hosts;
7. argv contains only the config path, instance selector, and process-local
   paths;
8. mutable `run-state.json` only on the control host;
9. a host-local deploy manifest;
10. process identity, stdout/stderr, and readiness files in the instance
    directory;
11. SHA-256 verification after configuration distribution;
12. a local profile lock plus a DB-scoped fence and execution gate;
13. fail-fast behavior for parallel stages;
14. collection of raw artifacts even on failure;
15. secrets supplied only through the environment and temporary mode 0600
    files.

### 8.2. Commands

```text
tpccctl validate
tpccctl plan
tpccctl deploy
tpccctl schema
tpccctl load
tpccctl check [--after-import|--after-run]
tpccctl start
tpccctl status
tpccctl stop
tpccctl collect
tpccctl consolidate
tpccctl run
tpccctl cleanup --yes
```

`run` executes:

```text
validate → deploy → schema → load → check(after-import)
→ arm workers → ramp-up → measurement → drain
→ check(after-run) → collect → consolidate
```

Individual skip flags are permitted; every skipped step is recorded in the
run-state and aggregate.

### 8.3. Profile and run-config

The profile is edited by a human. The orchestrator validates it, applies
built-in defaults for omitted workload parameters, and creates a normalized
`run-config.json` containing:

- schema version and run ID;
- profile and binary SHAs;
- the `tpcc-spec` binary SHA, the spec module SHA, and the `spec-state.json`
  SHA;
- DBMS kind and non-secret configuration;
- scale and warehouse assignment;
- workload parameters: transaction mix, think/keying times, terminals per
  warehouse, and related settings (defaults from orchestrator code unless
  overridden in the profile);
- an opaque generator/spec state reference;
- relative durations and phase policy;
- histogram schema;
- expected workers;
- retry/failure policy;
- artifact paths.

The orchestrator MUST NOT reject a profile because its workload parameters
differ from any TPC-C edition. Mix weights MUST be positive and sum to 100
(or an equivalent normalized representation); beyond that structural check,
parameter values are taken as configured.

The run-config contains no password; only the name of the environment variable
is stored. The profile contains the instance inventory but no assignment; all
ranges in the run-config are computed output of `balanced-contiguous-v1`.
Functional worker parameters MUST NOT be duplicated in argv. A worker is
launched as `tpcc-<dbms> worker --run-config <path> --instance <name>` and
selects its own assignment by `instance`.

Examples:

- [profile.v1.yaml](examples/profile.v1.yaml);
- [control-config.v1.json](examples/control-config.v1.json);
- [run-config.v1.json](examples/run-config.v1.json);
- [start-token.v1.json](examples/start-token.v1.json).

The examples are illustrative: values of the form `*_SHA256` are replaced by
the generator. A production hash is 64 lowercase hexadecimal characters
computed from canonical JSON according to RFC 8785 or from the original binary
bytes. Test fixtures MUST contain real, verifiable hashes.

The implementation MUST provide JSON Schema for the profile, control-config,
run-config, spec-state, start-token, readiness, process state, and results. The
YAML profile is validated as a JSON data model with
`additionalProperties:false`. Defaults are materialized into the local
immutable `control-config.json` and runtime `run-config.json`; the original
profile is not read thereafter. The control-config contains the SSH inventory,
local/state/result paths, and deploy policy. The run-config contains only
parameters for runtime hosts.

### 8.4. Directories

Runtime host:

```text
/home/user/portable-tpcc/
├── .tpccctl/deploy-manifest.json
├── bin/
├── schema/
└── runs/<run_id>/
    ├── run-config.json
    ├── spec-state.json
    ├── start-token.json
    ├── load-plan.json
    ├── loader/<name>/
    └── worker/<name>/
        ├── process.json
        ├── ready.json
        ├── armed.json
        ├── stdout.log
        ├── stderr.log
        ├── events.jsonl
        └── result.json
```

Control host:

```text
<state-dir>/
├── profiles/<profile-id>/current-run.json
├── profiles/<profile-id>/run.lock
└── runs/<run_id>/
    ├── run-state.json
    ├── control-config.json
    ├── profile.redacted.yaml
    ├── run-config.json
    ├── spec-state.json
    └── load-plan.json
```

### 8.5. Deploy and Cleanup

Deploy:

- verifies the source file hash;
- updates the host-local manifest incrementally;
- starts with `complete:false` and finishes with `complete:true`;
- a repeated deploy is idempotent.

Cleanup removes only paths from the complete manifest and never executes an
unconditional `rm -rf remote_root`. `--yes` is required in non-interactive
mode.

### 8.6. DB-scoped Fence

Before `schema`, `load`, `check`, or `start`, the control process obtains,
through `IAdminAdapter`, a fence on the adapter-discovered canonical database
identity. The identity MUST contain stable cluster/tenant/database IDs and
cannot consist solely of a user-provided endpoint alias.

The adapter implements the fence using an atomic technical metadata record
outside the benchmark tables. The record contains `run_id`, a random fencing
token, a generation, and `not_after`. Another profile/control process cannot
obtain the next generation until the current one expires. Each mutating admin
operation and load batch passes the generation; the database rejects stale
generations.

Before the workload starts, `not_after` MUST extend beyond the maximum drain
deadline with a safety margin. The worker checks the fence and execution gate
before ramp-up. After gate commit, another control process cannot obtain the
fence until the existing run completes, even if the first control process
fails. Loss or premature expiration of the fence causes the run to fail. The
metadata is not part of the measured schema and is removed only by the token
owner.

### 8.7. Clock Synchronization and Two-phase Start

Clock calibration uses multiple samples per host, selects the sample with the
minimum RTT, and stores the offset together with its uncertainty. The check is
repeated before and after measurement; the worker detects wall-clock steps and
enforces deadlines using a monotonic clock. The profile specifies maximum
skew, uncertainty, and drift.

Startup is divided into prepare and commit:

1. The control process distributes `run-config.json` and `spec-state.json`.
2. The worker verifies hashes, the DB fence, and the adapter, creates the
   runtime, and writes `ready.json`, but does not start the workload.
3. Once the ready set is complete, the control process creates
   `start-token.json`, bound to the config SHA, fence generation, and ready-set
   hash. The token contains future phase epochs and the expected generation of
   the DB-side execution gate.
4. The worker atomically accepts the token and writes `armed.json`.
5. Only after the armed set is complete and process heartbeats are current
   does the control process transition the shared execution gate from
   `prepared` to `committed` in a single database operation. The gate contains
   config/token/ready-set hashes, the fence generation, and `not_before`.
6. The worker admits the workload only after reading a `committed` gate with
   an exact match of the generation and hashes.

If the ready/armed set is incomplete, the control process cannot commit the
gate and no conforming worker starts the workload. The DB-side gate eliminates
partial updates of host-local files. After commit, loss of a worker does not
immediately stop the remaining processes, but it is recorded in
heartbeat/status and causes the final run to fail.

`ready.json` contains:

```json
{
  "schema_version": 1,
  "run_id": "20260728-lab-ydb",
  "instance": "worker-a",
  "instance_nonce": "...",
  "run_config_sha256": "...",
  "spec_state_sha256": "...",
  "binary_sha256": "...",
  "adapter": "ydb",
  "warehouse_ranges": [[1, 101]],
  "ready_at": "2026-07-28T11:59:20Z",
  "clock_calibration": {
    "measured_at": "2026-07-28T11:59:18Z",
    "offset_ms": 3,
    "uncertainty_ms": 8,
    "rtt_ms": 11
  }
}
```

### 8.8. Accounting at Phase Boundaries

The worker maintains separate raw populations by submit/start/complete times
and does not clear shared counters at the warmup boundary. Monotonic
timestamps, phase epochs, and the outcome are recorded for every logical
operation. Drain operations are accounted for separately.

The orchestrator and shared metrics code classify populations using the phase
epochs and accounting policy recorded in `run-config.json`. They do not apply
TPC-C edition qualification rules or move late completions between
populations outside that configured policy.

### 8.9. Process Supervision

The initial version uses `nohup`, but a PID is not considered sufficient
identity. `nohup` starts a small wrapper with a pre-generated instance nonce.
The wrapper obtains an exclusive instance lock, writes and `fsync`s
`process.json` with its PID, `/proc/self` start time, nonce, run/config hashes,
and generation, and then uses `exec` to start the workload binary with the
same PID. A registration failure prevents `exec`.

A repeated start first reconciles the remote record; a signal may be sent only
to a process with matching PID, start time, and nonce.

If the control process fails between launch and writing local state, the next
invocation recovers processes from remote records and the config hash. A stale
record is moved to artifacts rather than silently overwritten.

Stop:

1. sends SIGTERM to the worker;
2. the worker stops admission and performs drain;
3. waits for `stop_grace`;
4. sends SIGKILL if necessary;
5. verifies that the process is absent;
6. preserves partial artifacts.

A repeated stop is idempotent. Loss of a worker during measurement causes the
overall run to fail; reassignment is prohibited because it would change the
terminal population and timing.

### 8.10. States

```text
planned → deploying → schema → loading → checking_import
→ preparing → arming → ramping → measuring → draining
→ checking_result → collecting → consolidating → completed
```

Any state may transition to `stopping` and then `failed`. The run-state is
written atomically using a temporary file + rename and contains the most
recent error and all known processes.

## 9. Metrics and Consolidation

### 9.1. Worker Result

Each worker writes JSON containing:

- run/config/binary/profile SHAs;
- the spec module SHA;
- adapter and server version;
- assignment;
- actual phase timestamps;
- per-transaction counters according to the configured mix;
- retries by normalized class and native code;
- telemetry for asynchronous queues;
- response, DB-attempt, admission-wait, and end-to-end histograms;
- input statistics useful for engineering analysis;
- clock diagnostics;
- fatal errors.

A histogram stores counts for shared buckets in microseconds,
encoding/version, underflow/overflow, as well as a mergeable `count`, the exact
sum of durations, and the exact maximum. A worker is not the source of final
percentiles or averages.

### 9.2. Consolidation

`consolidate` MUST:

1. verify identical run IDs and config SHAs;
2. verify the complete expected worker set;
3. verify a complete, non-overlapping assignment;
4. verify identical histogram schemas;
5. sum bucket counts and counters;
6. compute percentiles only after merging;
7. compute throughput and response-time summaries from the merged data and
   the configured phase/accounting policy, without artificial clamping;
8. retain references to the original worker artifacts.

The following are prohibited:

- summing or averaging p99 values;
- scaling a partial result to account for missing workers;
- replacing missing samples with zeros;
- hiding outcomes;
- limiting a computed metric to an artificial maximum;
- emitting a TPC-C conformance or standard-qualification verdict.

### 9.3. Final Artifacts

```text
results/<run_id>/
├── raw/loader/<instance>/
├── raw/worker/<instance>/
├── orchestrator/
│   ├── profile.redacted.yaml
│   ├── run-config.json
│   ├── spec-state.json
│   ├── run-state.json
│   ├── start-token.json
│   └── load-plan.json
├── checks/
│   ├── after-import.json
│   └── after-run.json
├── collection-manifest.json
├── aggregate.json
└── summary.txt
```

`aggregate.json` is the canonical result. `summary.txt` only presents it and
contains no unique data.

Before collection, each process atomically publishes `artifact-manifest.json`
with the size and SHA-256 of each **payload** file, exit status, instance
nonce, and `finalized:true`; the manifest itself is not included in its own
payload. The collector first copies data to a temporary directory, then
verifies the manifest, and only then publishes the raw instance directory.

After collection, the control process atomically creates
`collection-manifest.json`, covering all process manifests, the control-config,
run/spec/start state, load plan, and check results. The aggregate is built only
from files in this manifest and stores its SHA-256. Unsealed data remains
marked `partial` and is excluded from the primary aggregate.

### 9.4. Infrastructure Health Flags

The orchestrator produces infrastructure flags:

```text
workers_complete
assignment_valid
clock_skew_valid
phase_boundaries_valid
post_import_checks_valid
post_run_checks_valid
no_ambiguous_commit
no_integrity_errors
no_drain_cancellations
artifacts_sealed
```

These flags describe run completeness and integrity only. The aggregate MUST
NOT include flags or a `qualified` verdict that assert compliance with any
TPC-C edition's parameter or metric requirements.

## 10. Errors, Recovery, and Idempotency

| Operation | Contract |
| --- | --- |
| `validate`, `plan` | no side effects |
| `deploy` | repeatable by manifest/hash |
| `schema` | checks existing state; destructive recreation only with an explicit flag |
| `load` | repeatable: unknown batches safely pass through `PutBatch` again |
| `start` | permitted only for the owner of the profile lock, DB fence, and prepared execution gate; another active run is prohibited |
| `stop` | repeatable; already stopped = success |
| `collect` | repeats the download into a temporary directory and publishes atomically |
| `consolidate` | a pure deterministic function of the artifacts |
| `cleanup` | only manifest-owned paths and explicit confirmation |

A partially successful parallel stage is considered failed. The orchestrator
attempts to stop processes that have already started and collect their logs. A
collection error does not overwrite the original cause of failure; it is added
as a separate cause.

## 11. Security

- DB and SSH passwords MUST NOT appear in profile artifacts, argv, logs,
  run-config, or run-state.
- A profile containing secrets is redacted before being stored.
- ssh-agent is preferred; direct use of passphrase-protected private keys is
  not required in the initial version.
- The database secret is passed as the name of an environment variable.
- When necessary, a remote secret is created with mode 0600, sourced by the
  wrapper shell, and deleted before `exec`.
- Host keys MUST be verified; `insecure_ignore_host_key` is permitted only when
  explicitly set in the profile and is recorded in the run-state.
- All paths are normalized relative to permitted roots; `..` and symlink
  escapes are rejected.
- Native driver logs are redacted for known connection-string forms.

## 12. Validation

`tpccctl validate` MUST reject:

- an unknown `apiVersion`, DBMS, or field;
- an instance name outside `[a-z][a-z0-9-]*`, or a duplicate name;
- a warehouse count outside the supported range;
- an empty loader/worker list, or more instances than warehouses;
- any manual `warehouse_ranges`, `assignment`, or `owns_global_data` in the
  profile;
- a transaction mix that is empty, contains a non-positive weight, or does not
  normalize to a complete distribution;
- zero or negative threads, pool, duration, timeout, or batch size;
- an incomplete computed assignment or more than one owner of the DB-wide data
  shard;
- reuse of a remote `(host, run_dir, instance)`;
- missing artifacts;
- a secret literal instead of `password_env`;
- credentials in an endpoint URL, connection string, or DBMS options;
- incompatible adapter capabilities/isolation;
- insufficient lead time;
- incompatible histogram schemas;
- a retry policy that permits replay after `ambiguous_commit`.

Validation MUST NOT reject a profile because workload parameters differ from
those of any TPC-C edition.

Adapter preflight checks the server version, permissions, connectivity,
isolation, schema state, and physical configuration.

## 13. Implementation Checks and Tests

### 13.1. Shared Unit Tests

- test vectors from the spec module;
- domain types and canonical encoding;
- immutable input across injected retries;
- transaction workflows through a fake adapter;
- `tpcc-spec` CLI/library equivalence;
- terminal identity and warehouse assignment;
- phase classification at boundary timestamps;
- histogram merging;
- load sharding independent of the number of shards.

### 13.2. Adapter Contract Suite

One test suite is run for each DBMS:

- DDL/create/clean;
- initial population hash/cardinality;
- interruption of `PutBatch` at different stages and safe retry;
- all operations exported by the spec module;
- transaction rollback atomicity;
- deadlock/serialization retry;
- ambiguous commit injection;
- asynchronous runtime operations;
- the complete catalog of shared checks;
- cancellation and reconnect policy.

### 13.3. Orchestrator Tests

- strict profile validation;
- materialization of built-in workload defaults and profile overrides (mix,
  think/keying times, and related settings);
- rejection of structurally invalid mixes without comparing them to any TPC-C
  edition;
- plan snapshots and argv;
- manifest-safe cleanup;
- redaction;
- config distribution/hash mismatch;
- deterministic assignment with uneven division and different profile ordering;
- rejection of a manual or corrupted assignment;
- DB-scoped fence collision and a stale generation from another profile;
- missing worker/early exit;
- incomplete ready/armed set and uncommitted execution gate;
- startup deadline;
- stop/drain;
- PID reuse and recovery after a control process crash;
- partial artifact collection;
- artifact manifest tampering;
- aggregate golden files without a TPC-C conformance verdict;
- integration testing through a local SSH target.

### 13.4. Cross-DB Equivalence

For a small shared seed:

1. load all three DBMSs;
2. compare canonical row hashes;
3. execute a fixed trace of logical inputs;
4. compare normalized outputs and checks;
5. separately permit only documented differences in physical metadata.

## 14. Proposed Repository Structure

```text
tpcc/
├── spec/
├── domain/
├── generator/
├── transactions/
├── runtime/
├── loader/
├── checks/
├── metrics/
├── dbms/
│   ├── ydb/
│   ├── pgsql/
│   └── oceanbase/
└── app/
    ├── ydb/
    ├── pgsql/
    └── oceanbase/
tools/
└── tpccctl/
docs/
├── specification.md
└── examples/
```

All C++ targets are described by `ya.make`. The Go orchestrator is built using
the existing Go support in `ya make`; no alternative root build system is
introduced.

## 15. Initial-version Completion Criteria

1. All three binaries pass the same adapter contract suite.
2. The same seed creates equivalent logical datasets.
3. Two or more worker hosts cover the warehouses without duplicating
   terminals.
4. Loss of a worker causes the run to fail but preserves partial artifacts.
5. Phases are synchronized, and warmup samples do not enter measurement.
6. Retry fault injection confirms input immutability.
7. All asynchronous workload operations have completion metrics.
8. Domain values retain precision at the adapter boundary.
9. Post-import and post-run checks succeed.
10. The aggregate is reproducible from raw artifacts without DBMS access.
11. The profile, run-config, and results contain no secrets.
12. `plan`, repeatable load, idempotent stop/collect, and manifest cleanup are
    covered by tests.

## 16. Open Decisions Before Implementation

The following must be decided before coding begins:

1. the specific C++ future/coroutine ABI for the shared libraries;
2. the histogram bucket layout and maximum measurable latency;
3. the ambiguous commit resolution strategy for each DBMS;
4. the canonical row encoding format for cross-DB hashes;
5. the minimum supported versions of YDB, PostgreSQL, and OceanBase;
6. the retention policy for asynchronous queues after a worker failure.

These decisions MUST be adopted as versioned ADRs before
`portable-tpcc/v1` is stabilized; they must not be encoded implicitly in the
first adapter.
