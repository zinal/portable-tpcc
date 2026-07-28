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

This document neither replaces nor restates the TPC-C standard. The schema
composition, data generation rules, transaction profiles, terminal model,
input distributions, response-time requirements, and standard metric formulas
are defined by the selected TPC-C edition. `portable-tpcc` stores the reference
and identifier for that edition in the run manifest. This document defines
only the architectural mechanisms for implementing these rules jointly across
multiple DBMSs and multiple generator hosts.

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
- Automated checks before and after the test.

### 2.2. Non-goals for the Initial Version

- Provisioning or administration of DBMS clusters.
- Requiring Kubernetes, Ansible, or systemd as the environment.
- A single universal SQL dialect.
- Support for server-side stored procedures as a portable interface.
- Automatic TPC result certification.
- Automatic continuation of measurement after a worker is lost.
- Dynamic terminal rebalancing during measurement.

`portable-tpcc` MUST call a result an official TPC-C result only after
independent verification of all TPC requirements. By default, the report contains
`result_class: engineering`.

### 2.3. Modes

`engineering` permits profile overrides of standard parameters for
diagnostics. Every such change MUST be explicitly listed in
`deviations`.

`conformance` prohibits overrides of standard parameters and requires the full
set of checks and artifacts defined by the selected edition of the standard.
Specific values are not duplicated in this specification; they are provided by
the versioned `tpcc/spec/<edition>` module. The mode name itself does not
constitute a claim that the result is certified.

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

The selected edition of the standard is represented by a single versioned C++
package. It contains only the machine-executable rules and test vectors needed
by the implementation; the external TPC-C document remains the normative
source.

The package is built as:

- a library linked by `domain`, workload binaries, and contract tests;
- the DBMS-neutral `tpcc-spec` CLI used by the Go orchestrator;
- JSON Schema for CLI inputs and outputs.

Minimum CLI:

```text
tpcc-spec describe --edition <id>
tpcc-spec materialize --edition <id> --scale <json> --seed-source <json>
tpcc-spec derive-terminals --spec-state <json> --assignment <json>
tpcc-spec expected-data --spec-state <json> --load-plan <json>
tpcc-spec qualify --spec-state <json> --aggregate-input <json>
```

All commands are pure functions and output canonical JSON. `describe` returns
an immutable edition ID, the URL of the normative document, its known SHA-256,
the module ABI version, and the module SHA. `materialize` creates an opaque
`spec-state.json`; the orchestrator does not interpret its internal parameters.

`tpccctl` MUST NOT implement TPC-C rules in Go. It invokes `tpcc-spec`,
validates the JSON Schema, and records the binary/module hash. At startup, the
workload binary verifies that the linked module SHA matches `spec-state.json`.

### 4.2. `tpcc/domain`

Independent of DBMS SDKs, networking, and the scheduler:

- a versioned representation of the requirements of the selected TPC-C edition;
- identifier types;
- exact numeric types;
- immutable transaction input and output types exported by the spec module;
- NURand and string generation;
- derivation of deterministic RNG streams;
- initial population rules;
- shared business calculations;
- invariants and expected cardinalities.

Scale and type constraints are defined by the versioned spec module. In C++,
exact decimal values are represented by checked fixed-point types; conversion
to `double` within the domain and adapter API MUST NOT be used.

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

- terminal state machines from the spec module assignment;
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

- the check catalog for the selected edition of the standard;
- a versioned identifier and expected result type for each check;
- checks for load completeness and shard consistency;
- statistical checks of the executed workload;
- infrastructure checks for phases, ownership, and artifacts.

The SQL/query for each condition is implemented by the adapter, but the
identifier, reference to the relevant section of the standard, expected
semantics, and result format are shared. The text of the standard condition is
not copied into either the library or this specification.

### 4.8. `tpcc/metrics`

- counters;
- histograms with shared boundaries;
- error/retry events;
- worker result serialization;
- deterministic merging;
- qualification flags.

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

The versioned spec module constructs the full set of standard terminal
identities for the specified scale. The user profile lists only loader/worker
instances and hosts; the profile contains neither manual warehouse ranges nor
a DB-wide data ownership flag.

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
- separate executors for asynchronous parts of the workload required by the
  standard;
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

Deferred operations required by the standard are implemented by the shared
runtime through a typed bounded queue and a separate executor. The adapter
executes only the atomic database portion of a queue item.

The queue records the logical ID and enqueue/start/completion times. At the
measurement boundary, admission stops and already accepted items receive a
separate drain window. The specific time limits and criteria come from the
versioned spec module rather than from this document.

## 6. Separation of Logical and Physical Schemas

### 6.1. Shared Schema Model

The versioned spec module describes tables, columns, constraints, and logically
required access paths in a DBMS-neutral AST. It is the sole source of the
schema for the generator, checks, and adapter contract tests.

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

- all checks marked `after-import` by the spec module;
- batch manifest completeness;
- absence of overlaps and gaps in the load assignment;
- correspondence of actual cardinalities to the expected values computed by
  the spec module for the scale;
- canonical sample hashes that are identical for all adapters;
- readiness of DBMS-specific indexes/statistics.

The report stores check identifiers, references to sections of the standard,
and machine-readable results. It does not copy normative TPC-C text.

## 8. The `tpccctl` Orchestrator

### 8.1. Principles

The control plane follows these principles:

1. one self-contained Go binary;
2. a declarative YAML profile, `portable-tpcc/v1`;
3. SSH + SFTP/tar without a mandatory agent;
4. `plan` without side effects;
5. an immutable `run-config.json` that is byte-identical on runtime hosts;
6. argv contains only the config path, instance selector, and process-local
   paths;
7. mutable `run-state.json` only on the control host;
8. a host-local deploy manifest;
9. process identity, stdout/stderr, and readiness files in the instance
   directory;
10. SHA-256 verification after configuration distribution;
11. a local profile lock plus a DB-scoped fence and execution gate;
12. fail-fast behavior for parallel stages;
13. collection of raw artifacts even on failure;
14. secrets supplied only through the environment and temporary mode 0600
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

Individual skip flags are permitted only in `engineering`; all skipped steps
are recorded as deviations.

### 8.3. Profile and run-config

The profile is edited by a human. The orchestrator validates it and creates a
normalized `run-config.json` containing:

- schema version and run ID;
- profile and binary SHAs;
- edition metadata, the `tpcc-spec` binary SHA, and the `spec-state.json` SHA;
- DBMS kind and non-secret configuration;
- scale and warehouse assignment;
- an opaque generator/spec state reference;
- relative durations and phase policy;
- histogram schema;
- expected workers;
- retry/failure policy;
- artifact paths.

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

Only `tpcc-spec qualify` determines which populations are included in standard
throughput and response-time metrics. The orchestrator does not encode these
rules or move late completions between populations.

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
- the TPC-C edition identifier and spec module SHA;
- adapter and server version;
- assignment;
- actual phase timestamps;
- standard workload counters according to the spec module schema;
- retries by normalized class and native code;
- telemetry for asynchronous queues;
- response, DB-attempt, admission-wait, and end-to-end histograms;
- input statistics required by the spec module to check the workload;
- clock diagnostics;
- fatal errors and deviations.

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
7. pass the merged data to the spec module to calculate standard metrics
   without artificial clamping;
8. calculate derived engineering metrics separately;
9. apply qualification rules;
10. retain references to the original worker artifacts.

The following are prohibited:

- summing or averaging p99 values;
- scaling a partial result to account for missing workers;
- replacing missing samples with zeros;
- hiding outcomes;
- limiting a computed standard metric to an artificial maximum.

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
marked `partial` but does not participate in the qualified aggregate.

### 9.4. Qualification Flags

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

Standard qualification flags are provided by the versioned spec module. The
final `qualified` value is the conjunction of the infrastructure flags and the
mandatory standard flags for the selected mode. The final JSON stores the
source (`orchestrator` or `spec:<edition>`) of each flag.

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
- Host keys MUST be verified; `insecure_ignore_host_key` is permitted only in
  `engineering` and is recorded as a deviation.
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
- zero or negative threads, pool, duration, timeout, or batch size;
- an incomplete computed assignment or more than one owner of the DB-wide data
  shard;
- reuse of a remote `(host, run_dir, instance)`;
- missing artifacts;
- a secret literal instead of `password_env`;
- credentials in an endpoint URL, connection string, or DBMS options;
- incompatible adapter capabilities/isolation;
- disabling a required spec module runtime subsystem in conformance mode;
- insufficient lead time;
- incompatible histogram schemas;
- a retry policy that permits replay after `ambiguous_commit`.

Adapter preflight checks the server version, permissions, connectivity,
isolation, schema state, and physical configuration.

## 13. Implementation Checks and Tests

### 13.1. Shared Unit Tests

- test vectors from the versioned spec module;
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
- all operations exported by the selected spec module;
- transaction rollback atomicity;
- deadlock/serialization retry;
- ambiguous commit injection;
- asynchronous runtime operations;
- the complete catalog of shared checks;
- cancellation and reconnect policy.

### 13.3. Orchestrator Tests

- strict profile validation;
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
- normative stop/drain;
- PID reuse and recovery after a control process crash;
- partial artifact collection;
- artifact manifest tampering;
- aggregate golden files;
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
7. All asynchronous operations required by the spec module have completion
   metrics.
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
3. the set of supported edition packages and their update rules;
4. the ambiguous commit resolution strategy for each DBMS;
5. the canonical row encoding format for cross-DB hashes;
6. the minimum supported versions of YDB, PostgreSQL, and OceanBase;
7. the retention policy for asynchronous queues after a worker failure.

These decisions MUST be adopted as versioned ADRs before
`portable-tpcc/v1` is stabilized; they must not be encoded implicitly in the
first adapter.
