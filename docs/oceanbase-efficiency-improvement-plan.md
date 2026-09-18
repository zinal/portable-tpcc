# OceanBase TPC-C efficiency improvement plan

- Status: proposed
- Scope: `tpcc/dbms/oceanbase`, shared transaction/runtime telemetry where needed
- Reference workload: `w45k06`, 45,000 warehouses, profile `profile-21n.yaml`

## 1. Goal

Improve the OceanBase worker so that a paced 45,000-warehouse run is limited
by useful database work or provisioned OceanBase capacity, rather than by
client-side round trips, excessive transaction concurrency, long lock critical
sections, or misleading telemetry.

The work must preserve:

- the shared TPC-C transaction semantics and fixed inputs across retries;
- atomic commit/rollback behavior;
- the normalized error model, including no blind retry after an ambiguous
  commit;
- the warehouse assignment and remote-transaction distributions;
- post-load and post-test integrity checks;
- the existing asynchronous `ITpccTransaction` contract: connector I/O may
  block only on the bounded I/O executor, never on a scheduler thread.

This remains an engineering TPC-C implementation, not an official audited
TPC-C result.

## 2. Observed baseline

### 2.1 Effective layout

The supplied profile has:

- 45,000 warehouses;
- 60 worker processes, therefore 750 warehouses and 7,500 terminals per
  worker;
- four worker processes per each of 15 client hosts;
- four coroutine scheduler threads per process;
- `max_inflight_per_worker: 256`;
- up to 15,360 OceanBase sessions and 15,360 client I/O threads globally;
- up to 1,024 sessions and I/O threads per client host;
- 63 binding HASH partitions;
- foreign keys disabled;
- `query_timeout: 3600`, which applies to bulk/check sessions but not worker
  OLTP sessions;
- normal pacing and exponential think time.

Consequently, the earlier risks of 45,000 automatically derived partitions
and foreign-key overhead do **not** apply to this run. The 63-partition schema
must remain one of the controlled experiment variables, but it is not an
obvious configuration error. `SHOW CREATE TABLE` must still confirm that the
existing schema was created from these options.

Both integrity checks are disabled in the baseline profile. The deprecated
`checks.after_run` alias is accepted as `after_test`, but its value is false.
Optimization acceptance runs must enable `after_import: true` and
`after_test: true`.

### 2.2 Representative worker result

One worker reported:

| Metric | Baseline |
| --- | ---: |
| Warehouses | 750 |
| Measurement interval | 1,200 s |
| New-Order throughput | 1,727.70 tpmC |
| Efficiency | 17.9% |
| Completed transactions | 76,851, including intentional New-Order rollbacks |
| Failed transactions | 188 |
| New-Order failed | 87 |
| Payment failed | 83 |
| Delivery failed | 18 |
| Scheduler ready at shutdown | 57 |

The theoretical paced ceiling for this worker is:

```text
750 × 12.86 = 9,645 tpmC
```

At the same per-worker rate, the 60-worker aggregate is approximately:

```text
1,727.70 × 60 = 103,662 tpmC
```

against a 45,000-warehouse paced ceiling of:

```text
45,000 × 12.86 = 578,700 tpmC
```

The progress samples showed `Inflight:256`, so the admission and connection
pool cap was saturated. From approximately 76,851 completed transactions over
1,200 seconds, one worker completed about 64 transactions/s. Little's law
therefore gives roughly four seconds of residence time per admitted
transaction:

```text
256 / 64 txn/s ≈ 4.0 s
```

This is consistent with an I/O/lock-bound client and inconsistent with a lack
of offered work. Low client or observer CPU utilization does not imply an idle
workload while every admitted transaction is suspended on a query, lock, or
commit.

### 2.3 Error behavior

The dominant reported errors are:

- `1205 Lock wait timeout exceeded`;
- `6235 can't serialize access for this transaction`.

Both are correctly mapped to `retryable_abort`. The terminal rolls the failed
attempt back, reuses the same logical inputs, applies bounded exponential
backoff with full jitter, and tries at most four times. Exhausted attempts are
counted as failed without stopping the run.

The retry mechanism is therefore not the primary defect. Increasing retries
would mostly hide contention while consuming more admission time.

### 2.4 Telemetry limitations exposed by the result

The final line:

```text
Measured Duration: 1200.0s (configured: 600s)
```

contains a display defect. The orchestrated worker correctly uses
`phases.measurement_ms` (20 minutes), but `TRunConfig::RunDuration` is not
populated by the worker role and retains its legacy 600-second default. The
throughput denominator uses the correct 1,200-second phase interval.

The repeated latency values such as `67108.9ms` are also not precise
percentiles. The default histogram is in microseconds with only a 4,096-unit
linear region and then power-of-two buckets. `67108.9ms` is a bucket upper
bound, not evidence that many transactions took exactly that time. Values
beyond the configured 120-second highest value share an overflow bucket, whose
reported percentile may become the maximum recorded sample. Current console
percentiles cannot reliably guide optimization at the observed latency.

These issues do not explain low throughput, but they must be fixed before
performance changes can be evaluated.

## 3. Root-cause model

### 3.1 Excessive sequential round trips

OceanBase reports `ExecuteBatchOptimized = false`, and `ExecuteBatch` folds
over semantic operations one at a time. A typical valid New-Order requires:

- one customer read;
- one warehouse read;
- district `SELECT FOR UPDATE` plus update;
- `oorder` plus `new_order` inserts;
- 5–15 individual item reads;
- 5–15 individual stock locking reads;
- 5–15 individual stock updates;
- 5–15 individual order-line inserts;
- commit.

An average ten-line New-Order therefore takes about 47 serialized
client/server exchanges. A full Delivery can require about 70 exchanges.
Every exchange adds network, OBProxy, executor-queue, plan lookup, and observer
queue latency.

This also multiplies lock duration: locks taken near the start of a transaction
remain held while all later exchanges complete.

### 3.2 Long lock critical sections

The principal hot locks are:

- `district(d_w_id, d_id)` reserved by New-Order;
- `warehouse(w_id)` and `district(d_w_id, d_id)` updated near the beginning of
  Payment;
- stock rows locked by New-Order;
- up to ten oldest `new_order` rows locked by Delivery before its update phase;
- customer rows updated by Payment and Delivery.

Payment holds the warehouse lock while it performs district update, address
reads, customer lookup/update, history insert, and commit. Delivery may acquire
ten `new_order` locks and then execute dozens of statements. Once base query
latency rises, these critical sections overlap, creating lock convoys. The
convoys further increase latency and eventually reach OceanBase's effective
lock/query deadline.

### 3.3 Concurrency above the useful knee

At 750 warehouses, a healthy paced worker needs approximately:

```text
750 × 10 terminals / 21 s ≈ 357 txn/s
```

At 100 ms mean database latency this needs about 36 concurrent transactions;
at 500 ms, about 179. A cap of 256 is useful only if high latency is intrinsic.
If the latency is caused by contention, 256 concurrent transactions per
process aggravate the cause.

Four processes per client host also create 1,024 blocking connector I/O
threads. This is a large scheduling and memory footprint even though the
coroutine scheduler uses only 16 threads per host.

### 3.4 Payment customer conflict handling

Payment reads a customer without a locking clause and later writes absolute
balance, YTD-payment, and payment-count values. Under concurrent updates,
OceanBase must reject one snapshot with a serialization failure to prevent a
lost update. The retry is correct but expensive.

The implementation needs either:

- a Payment-specific locking customer read, including deterministic selection
  of the median customer for last-name lookup; or
- an atomic increment update that returns or otherwise safely obtains the
  resulting values, with a protected read-modify-write path for bad-credit
  `c_data`.

The selected approach must be implemented consistently with the shared
semantic operation contract and tested for concurrent Payment/Delivery
updates.

### 3.5 Physical distribution

The current 63-way binding tablegroup co-locates equal warehouse hashes across
warehouse-scoped tables and may be appropriate for this deployment. It still
requires verification:

- each of the 63 partition leaders must be distributed across the intended
  tenant units and observers;
- worker traffic through port 2883 must not be pinned by OBProxy routing to a
  subset of leaders;
- tenant CPU, memory, log, network, compaction, and session limits must have
  headroom;
- statistics and plans must show single-partition access for local
  transactions;
- remote New-Order and Payment must be the expected minority of distributed
  transactions.

Changing the partition count without checking leader placement is not an
optimization plan.

### 3.6 Worker timeout is not the profile bulk timeout

`database.options.query_timeout: 3600` configures load, index, statistics,
integrity-check, and pre-flight sessions. The worker connection pool does not
call `ConfigureBulkLoadSession()` and retains OceanBase's OLTP session
defaults, commonly a 10-second `ob_query_timeout`.

OceanBase computes the effective lock wait from transaction, lock, and query
deadlines. A long lock convoy can therefore surface as 1205 even though the
profile appears to specify one hour. This fail-fast policy is allowed by the
project specification, but the effective worker values need to be observable.
Raising them before shortening transactions risks replacing visible timeouts
with larger invisible queues.

## 4. Work plan

### Phase 0 — Establish a trustworthy baseline

#### 0.1 Correct duration reporting

- Populate the worker's configured duration from
  `PhasePolicy.MeasurementMs`, or label/remove the legacy `RunDuration` value
  for orchestrated execution.
- Add a regression test for a 20-minute orchestrated measurement.
- Ensure stdout, `result.json`, and `aggregate.json` report the same interval.

#### 0.2 Make latency distributions useful

- Replace the coarse `linear_exp` latency histogram with a representation that
  has bounded relative error across at least 1 microsecond to 15 minutes, for
  example HdrHistogram-style significant digits.
- Preserve raw count, min, max, and sum.
- Export separate distributions for:
  - admission wait;
  - session-pool wait;
  - transaction duration including retries;
  - individual attempt duration;
  - workflow/database duration;
  - retry backoff;
  - commit duration.
- Keep artifact merge deterministic and validate histogram layout compatibility
  during consolidation.

#### 0.3 Add OceanBase operation telemetry

For every `EObQueryId`, collect without logging parameters:

- executions, failures, 1205, 6235, and other native error codes;
- p50/p90/p99 elapsed time;
- executor queue time;
- rows read/affected;
- transaction type and attempt number.

Add pool gauges:

- checked out, free, and pending waiters;
- connection replacements;
- time waiting for a session;
- current and peak I/O executor queue depth.

Artifacts must not contain connection strings, credentials, customer data, or
literal SQL parameters.

#### 0.4 Capture server-side evidence

For every benchmark point, collect a synchronized OceanBase snapshot:

- `GV$OB_SQL_AUDIT` grouped by SQL ID, plan ID, server, error code, and event;
- `GV$OB_LOCK_WAITS` and blocking row/transaction information;
- plan-cache hit/miss and plan changes;
- local versus remote/distributed transaction counts;
- partition leader distribution;
- tenant CPU, memory, session count, log bandwidth, RPC/network, minor freeze,
  compaction, and throttling metrics.

Run the sequential probe before and after each build:

```bash
./mind-tpcc debug --run-id w45k06 --profile profile-21n.yaml --repeats=30
```

The probe separates single-session statement/plan cost from concurrency and
lock cost.

#### Phase 0 exit criteria

- duration agrees across logs and artifacts;
- percentile error is at most 2% over the configured range and overflow is
  reported explicitly;
- pool wait, attempt time, retry time, commit time, and query-ID latency can be
  reconciled with total response time;
- the top blocking SQL and row-key classes for 1205/6235 are known;
- three unchanged baseline runs have aggregate tpmC within 5%.

### Phase 1 — Find the useful concurrency point

This phase changes configuration only. Keep schema, binary, pacing, placement,
and workload seed fixed.

Run a controlled matrix:

| Variable | Values |
| --- | --- |
| `max_inflight_per_worker` | 32, 64, 100, 128, 192, 256 |
| workers per client host | 2 and 4 |
| scheduler threads per worker | auto, 2, 4 |

Start with 60 workers and vary inflight. Then compare 30 workers owning 1,500
warehouses each, so global session count and process overhead can be varied
independently.

For every point record:

- aggregate and per-worker tpmC/efficiency;
- completed, retried, exhausted, and user-aborted counts;
- latency components and query-ID distributions;
- global sessions and client I/O threads;
- observer utilization and lock-wait totals.

Do not select the highest throughput point if it has unstable tail latency or
an increasing exhausted-retry rate. Select the smallest concurrency that is
within 3% of the best stable throughput.

#### Phase 1 exit criteria

- a reproducible concurrency knee is identified;
- no worker remains at its inflight cap for more than 10% of measurement time
  unless observers are demonstrably saturated;
- client-host runnable queue, context switching, and memory do not indicate
  overload;
- the selected point has lower p90 and retry rate than 256 inflight.

### Phase 2 — Optimize New-Order batches

Implement an OceanBase-specific optimized path while preserving the shared
workflow.

#### 2.1 Item lookup

- Deduplicate item IDs.
- Fetch all 5–15 items in one prepared statement.
- Validate exact result cardinality.
- Preserve the intentional invalid-item rollback profile: all valid lines must
  still perform their stock and order-line work before the invalid item causes
  the confirmed rollback.

Use a bounded family of prepared statements by item count or another bounded
binding mechanism. Do not create an unbounded SQL-text/statement-cache key
space.

#### 2.2 Stock locking

- Deduplicate stock keys.
- Sort keys by `(s_w_id, s_i_id)` before locking to establish a deterministic
  global lock order.
- Lock and fetch the set in one operation if OceanBase can guarantee lock
  acquisition in the required order; otherwise use ordered, pipelined groups
  that retain deterministic acquisition.
- Validate that every requested stock row was returned.

#### 2.3 Stock updates and order-line inserts

- Replace per-row stock updates with bounded array binding, a set-oriented DML
  statement, or a stored routine.
- Replace per-row order-line inserts with one multi-value/array-bound insert.
- Check affected-row cardinality and convert mismatches to integrity errors.
- Preserve exact decimal handling and `s_ytd`, order-count, and remote-count
  semantics.

Set `ExecuteBatchOptimized = true` only after all advertised batch operations
actually use the optimized path.

#### 2.4 Optional fused New-Order routine

After set-oriented operations are correct, evaluate a prepared stored routine
that performs the full New-Order transaction server-side. Adopt it only if:

- deployment/version compatibility is explicit;
- logical inputs and intentional rollback behavior remain identical;
- commit outcome and native errors remain visible to the adapter;
- routine installation is versioned and validated at schema/pre-flight time;
- integrity checks pass against both routine and non-routine paths.

#### Phase 2 exit criteria

- average valid ten-line New-Order uses at most 10 client/server exchanges;
- sequential-probe warm New-Order latency improves by at least 3×;
- under load, New-Order p90 improves by at least 2×;
- New-Order 1205/6235 rate per attempt falls by at least 80%;
- transaction workflow unit tests, OceanBase adapter tests, and all
  after-test integrity checks pass.

### Phase 3 — Shorten Payment and Delivery critical sections

#### 3.1 Payment

- Combine warehouse/district update and immutable location reads into the
  fewest supported server exchanges.
- Introduce a Payment-specific customer operation that prevents stale
  read/absolute-write conflicts.
- Keep bad-credit `c_data` truncation and history data exactly compatible with
  the shared workflow.
- Fuse history insert and commit where the commit outcome remains observable.

Target no more than three server exchanges for a normal Payment, excluding
session acquisition.

#### 3.2 Delivery

Implement a specialized OceanBase Delivery batch/routine that:

- selects exactly the oldest pending order independently for each district;
- prevents two Delivery transactions from committing delivery of the same
  order;
- updates `new_order`, `oorder`, `order_line`, and customer atomically;
- validates one deleted `new_order`, one updated order, expected order-line
  count, and one customer per processed district;
- uses a deterministic lock order;
- does not use `SKIP LOCKED` unless equivalence to the required oldest-order
  semantics is proven.

The preferred implementation performs all ten districts inside one server
call or a small bounded number of calls. Merely raising lock timeout is not a
solution.

#### Phase 3 exit criteria

- normal Payment uses at most three exchanges;
- full Delivery uses at most five exchanges;
- sequential-probe warm Payment and Delivery improve by at least 3×;
- no double-delivery or lost customer update is possible in a targeted
  concurrency test;
- Payment and Delivery exhausted retries are each below 0.01% of submitted
  business transactions at the selected concurrency.

### Phase 4 — Configuration and pre-flight safeguards

#### 4.1 Global session budget

Make `mind-tpcc plan` print:

- warehouses and terminals per worker;
- processes per host;
- per-host and global maximum sessions;
- per-host and global OceanBase I/O threads;
- estimated prepared-statement handles.

Warn when:

- more than 512 OceanBase sessions/I/O threads are configured on one client
  host without an explicit override;
- global sessions exceed a configured tenant budget;
- inflight per 750-warehouse paced shard is above 128 without an override.

Warnings must be advisory because actual capacity is deployment-specific.

#### 4.2 Partition defaults

- Preserve explicit `partitions: 63` for the reference run.
- Fix the inconsistency where explicit partition counts above 8192 are rejected
  but `partitions: 0` can derive a larger value.
- At large scale, require an explicit partition count or cap derivation with a
  clear warning.
- Print the resolved count and expected warehouse-to-partition ratio in
  `plan`, schema, and aggregate artifacts.

#### 4.3 Timeout policy

Keep bulk/check timeout separate from worker OLTP timeout. If an OLTP timeout
option is introduced:

- record it in run artifacts;
- set query, transaction, and lock timeout intentionally and document their
  interaction;
- use timeout as a safety bound, not a throughput tuning mechanism.

#### Phase 4 exit criteria

- the supplied 60-worker profile reports 15,360 global sessions and 1,024
  sessions per host before launch;
- unsafe auto-partition derivation cannot happen silently;
- effective OLTP timeout values are visible in ready/result artifacts.

### Phase 5 — Correctness, performance, and scale validation

Add automated tests for:

- exact item/stock result cardinality;
- duplicate item and stock keys;
- deterministic stock lock order;
- invalid-item rollback after all valid lines execute;
- concurrent New-Order on one district;
- concurrent Payment on one customer;
- Payment concurrent with Delivery on one customer;
- concurrent Delivery on one warehouse;
- affected-row mismatches;
- 1205/6235 rollback and fixed-input retry;
- connection loss during operation versus ambiguous commit;
- histogram accuracy and measurement-duration reporting.

Validation ladder:

1. unit tests and adapter SQL/binding tests;
2. single-session `debug --repeats=100`;
3. one warehouse with intentionally high contention;
4. 63 partitions and 750 warehouses on one worker;
5. 7,500 warehouses on ten workers;
6. full 45,000 warehouses on 60 workers;
7. repeat the final point at least three times.

For each step run after-import/after-test checks. Performance results are not
accepted if checks are skipped or fail.

## 5. Overall success criteria

The implementation is successful on the unchanged reference deployment when
all mandatory criteria hold for three consecutive 20-minute measurements
after a 10-minute ramp:

### Correctness

- all post-import and post-test integrity checks pass;
- intentional New-Order abort rate remains statistically consistent with 1%;
- no ambiguous commit is blindly retried;
- transaction mix and remote/local input distributions remain within expected
  statistical bounds.

### Reliability

- exhausted transaction failures are at most 0.01% of submitted business
  transactions, both globally and per type;
- retryable attempts are at most 1% of total attempts;
- no worker exits, connection-replacement storm, tenant-memory error, or
  histogram overflow occurs.

### Latency

- admission/session-pool wait p90 is below 100 ms;
- New-Order and Payment full transaction p90 are below 5 s;
- Order-Status and Stock-Level full transaction p90 are below 5 s;
- synchronous Delivery p90 is below 5 s as an engineering target;
- p99 for every type is below 2× its p90;
- reported percentiles have at most 2% relative quantization error.

### Throughput and resource use

- aggregate throughput is at least 347,220 tpmC, or 60% efficiency, as the
  minimum acceptance gate;
- aggregate throughput is at least 463,000 tpmC, or 80% efficiency, as the
  target;
- no individual healthy worker is below 90% of the fleet median tpmC;
- the workload does not remain pinned to its client inflight cap;
- increasing useful concurrency up to the selected knee increases throughput;
  above the knee, the selected lower setting is retained;
- if the 80% target cannot be reached because OceanBase resources saturate,
  the implementation is accepted as no longer client-bound only when server
  evidence shows a stable resource ceiling, lock/retry criteria still pass,
  and adding server capacity produces monotonic throughput scaling.

The minimum 60% gate represents a 3.35× improvement over the 17.9% baseline.
The 80% target represents approximately a 4.47× improvement.

## 6. Changes that are not sufficient

The following actions alone do not constitute success:

- increasing `max_attempts`;
- increasing `ob_query_timeout`, transaction timeout, or lock timeout;
- increasing `max_inflight_per_worker` above 256;
- adding worker processes while preserving excessive global sessions;
- disabling required locking or affected-row checks;
- using `SKIP LOCKED` to suppress Delivery waits without proving oldest-order
  equivalence;
- reporting higher tpmC with failed integrity checks, changed pacing, changed
  mix, or omitted failures;
- accepting coarse/overflowed latency percentiles.

## 7. Expected implementation order

1. Observability and baseline correction.
2. Concurrency matrix with the current SQL implementation.
3. New-Order set-oriented item, stock, and order-line operations.
4. Payment customer conflict and round-trip reduction.
5. Delivery server-side batch with strict cardinality guards.
6. Configuration safeguards.
7. Full correctness and 45,000-warehouse validation.

This order establishes evidence before invasive SQL changes, captures
low-risk tuning gains early, and concentrates implementation work on the
transactions that currently exhaust retries and hold locks longest.
