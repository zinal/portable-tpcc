# Implementation Specification: Horizontal Scaling of Driver and Tier-A

This document describes the **specific changes** required in the `dbt5` / EGen integration to run multiple instances of `Driver`, `BrokerageHouse`, and `MarketExchange` without application-side bottlenecks and **without violating TPC-E requirements** (v1.14.0), for the client-side business-logic variant (`BrokerageHouseMain -1`, without stored procedures).

Related components:

| TPC-E role | Program | Directory |
| --- | --- | --- |
| Driver / CE (+ optional DM) | `Driver.exe` | `dbt5/Driver`, `dbt5/common/Driver.*`, `dbt5/common/Customer.*` |
| Driver / MEE | `MarketExchange.exe` | `dbt5/MarketExchange`, `dbt5/common/MarketExchange.*` |
| Tier-A (SUT) | `BrokerageHouseMain-pgsql.exe` | `dbt5/pgsql_bin/BrokerageHouse`, `dbt5/common/BrokerageHouse.*` |
| Tier-B (SUT) | PostgreSQL | `sql/pgsql/` |

---

## 1. Goal

Provide compliant horizontal scaling:

1. **CE** — multiple processes/hosts with optional C_ID partitioning.
2. **DM** — exactly one instance for the entire Test Run.
3. **MEE** — multiple instances with unique `UniqueId` values and coordinated `SetBaseTime`.
4. **Tier-A (`BrokerageHouse`)** — a pool of identical processes; clients (CE/DM/MEE and BH→MEE) connect to them through a **connection pool with round-robin selection** (§5.4).
5. The performance bottleneck should move to Tier-B (the database), rather than remain in the accept loop or CPU of a single Driver or BH process.

Non-goal: semantic application or database sharding by packet content (by `C_ID` / transaction type in the Connector).

---

## 2. TPC-E Constraints (Mandatory)

The changes MUST preserve the following specification rules.

### 2.1. Roles and Network

- **Driver** = Driving & Reporting + EGenDriver (CE/MEE/DM) + upstream Connector.
- **Tier-A** = downstream Connector + EGenTxnHarness + Frame Implementation + Database Interface.
- **Tier-B** = Database Server.
- A network connection between Driver and Tier-A is mandatory (Clause 4).
- Response Time is measured at the Driver (`sTn` / `eTn`).

### 2.2. CE

- Multiple CE instances may run concurrently; each MUST preserve the mix.
- C_ID partitioning **is allowed** (Clause 6.3.2 / 4.4.1.2) if:
  - the subrange is contiguous;
  - the minimum C_ID is the start of a Load Unit (`…001`);
  - the subrange size is ≥ 5000 and is a multiple of the Load Unit size (1000);
  - different CEs may use different sizes;
  - `iPartitionPercent = 50` for a compliant run;
  - the Load Units produce approximately equal numbers of Trade-Result transactions during the Measurement Interval (verified by EGenTester).
- With partitioning, EGen selects a C_ID from the partition 50% of the time and from the entire Configured Customer range 50% of the time.

### 2.3. DM

- Exactly **one** Data-Maintenance Generator is allowed for the entire compliant Driver.
- Trade-Cleanup runs once before the Test Run starts (initiated by the DM).

### 2.4. MEE

- Multiple MEE instances are allowed (EGenDriverMEE; in EGen these are instances of the `CMEE` class).
- Each MUST have a unique `UniqueId` (for RNG seeds).
- `SetBaseTime()` MUST be called in a coordinated manner on all instances before transactions start.

### 2.5. Routing Prohibitions

- **4.4.1.3 (no-peeking-in-the-packet):** the Connector between Driver and EGenTxnHarness MUST NOT route based on the contents of the transaction packet.
- **4.4.1.4 / 2.5:** Frame Implementation does not perform routing; data access transparency applies.
- Therefore, load distribution for CE/DM/MEE ↔ BH and BH ↔ MEE MUST use only a **pool of pre-established connections** with a **per-operation round-robin policy**, without selecting an endpoint based on message fields (see §5.4). External load balancers (L4/L7 VIPs, etc.) MUST NOT be used in the test environment architecture.

### 2.6. Other Constraints

- All Driver instances use identical `EGenInputFiles`, scale factor, and configured/active customer values.
- A manually specified RNG seed (`Driver -r`) makes the run invalid; preserve this behavior.
- Database data copies are prohibited on the Driver, except for `EGenInputFiles`.

---

## 3. Target Deployment Topology

```text
DRIVER (outside the SUT)
┌────────────────────────────────────────────────────────────┐
│  CE₁ … CEₙ   Driver.exe --run-config … --role ce --instance ceN …   │
│  DM          Driver.exe --run-config … --role dm --instance dm0 …   │
│  MEE₁ … MEEₘ MarketExchange.exe --run-config … --instance meeM …    │
│  conn pool → BH₁…BHₖ  (per-operation RR, init + reconnect)  │
└───────────────┬────────────────────────────▲───────────────┘
                │ CE/DM → BH                 │ TR/MF ← MEE
                │ (connection pool, RR)      │ (connection pool, RR)
                ▼                            │
TIER-A (SUT)
┌────────────────────────────────────────────────────────────┐
│  BH₁ … BHₖ   BrokerageHouseMain --run-config … --instance bhK …     │
│  worker = incoming socket; own libpq; TxnHarness + SQL     │
│  conn pool → MEE₁…MEEₘ  (send_to_market, per-operation RR) │
└────────────────────────────┬───────────────────────────────┘
                             │ SQL frames
                             ▼
TIER-B — one logical PostgreSQL database
```

Normative startup order:

1. Start PostgreSQL (with the schema loaded).
2. Start `BH₁ … BHₖ`; each BH opens its listen socket, initializes its outgoing pool to MEE, creates its ready file, and only then enters its `accept()` loop (§5.4.3).
3. After all BH instances are listen-ready (a TCP connect to each listen port succeeds), start `MEE₁ … MEEₘ`; each MEE opens its listen socket, initializes its outgoing pool to BH, waits for the shared epoch (if scheduled), creates its ready file, and then enters its `accept()` loop.
4. Wait for all MEE and BH instances to become fully service-ready (§5.4.3). The BH↔MEE cyclic dependency is resolved by opening listen sockets before outgoing-pool initialization; inbound bootstrap connections queue in the kernel TCP backlog until the listener enters `accept()`, rather than being actively accepted during pool initialization.
5. Start one Driver instance with role `dm` → wait for the stable Trade-Cleanup completion marker.
6. Start `CE₁ … CEₙ` → ramp-up → Measurement Interval.
7. Stop CE → drain MEE while DM/BH/MEE remain running → stop DM → MEE → BH → collect logs.

---

## 4. Implementation Status

Most integration work in §5–§6 is already present in `dbt5/common` (`StartupCli`, `RunConfig`, `EndpointConnectionPool`, role-aware `CDriver`, and related modules). EGen already provides the required primitives: the partitioned `CCE` constructor, `CDriverCEPartitionSettings`, `CMEE(UniqueId)`, and `SetBaseTime()`.

### 4.1. Resolved Gaps (formerly G1–G4, G6)

| # | Was | Resolution | Location |
| --- | --- | --- | --- |
| G1 | C_ID partitioning was not exposed through the CLI or passed to `CCustomer` | CLI flags `--ce-start-id`, `--ce-part-count`, `--ce-part-percent`; partitioned `CCE` path in `CCustomer` with local validation (including minimum size 5000) | `dbt5/common/StartupCli.cpp`, `Customer.cpp`, `Driver.cpp` |
| G2 | Every `Driver.exe` always created a DM and ran Trade-Cleanup | Role/mode-aware execution path: manual `--mode {all\|ce\|dm}` and orchestrated `--role {ce\|dm\|standalone}` with `--run-config` | `dbt5/common/StartupCli.cpp`, `Driver.cpp` |
| G3 | The MEE `UniqueId` was hard-coded to `1` | `-U` / `--unique-id` from argv; required under `--run-config` | `dbt5/common/StartupCli.cpp`, `MarketExchangeMain.cpp` |
| G4 | BH/MEE used a single host:port; there was no round-robin connection pool | Shared `CEndpointConnectionPool` with per-operation round-robin, deferred-accept bootstrap (listen socket before outgoing pool; `accept()` loop starts only after pool init and ready file), and background reconnect | `dbt5/common/EndpointConnectionPool.*`, `CESUT`, `DMSUT`, `MEESUT`, `TxnHarnessSendToMarket`, `BrokerageHouse.cpp`, `MarketExchange.cpp` |
| G6 | CE-thread UniqueIds were process-local (`1..iUsers`); multiple processes could produce seed collisions | `--ce-id-base`; each CE thread receives `UniqueId = ce-id-base + slot`, with slot index kept separate from global UniqueId. Under `--run-config`, `--ce-id-base` is required; manual multi-CE startup still defaults to base `1`, so operators must assign non-overlapping bases | `dbt5/common/StartupCli.cpp`, `Driver.cpp` |

### 4.2. Remaining Gaps

| # | Problem | Location |
| --- | --- | --- |
| G5 | There is no multi-host orchestrator implementation; its target contract is specified separately | `docs/spec-orchestrator.md` (`tpcectl`) |

---

## 5. Required Changes

### 5.1. Driver: `all` / `ce` / `dm` Modes

**Files:** `dbt5/Driver/DriverMain.cpp`, `dbt5/common/Driver.{h,cpp}`.

**Behavior:**

| Mode | CLI | CE threads | DM thread | Trade-Cleanup |
| --- | --- | --- | --- | --- |
| `all` (default, compatibility) | no flag / `--mode all` | yes (`-u`) | yes | yes |
| `ce` | `--mode ce` | yes (`-u` required) | no | no |
| `dm` | `--mode dm` | no | yes | yes (before the DM loop) |

**Requirements:**

1. In `ce` mode, do not construct `CDM` / `CDMSUT`, call `DoCleanupTxn()`, or start the DM thread.
2. In `dm` mode, do not create CE user threads; do not require `-u` (or ignore it).
3. Explicitly print the selected mode in usage output and logs.
4. Document that the test environment includes exactly one DM-capable Driver process: either one `--mode dm` process or one `--mode all` process. When `--mode all` is used, every other Driver process MUST use `--mode ce`.

**Normative CLI:**

```text
# Manual/legacy startup:
--mode {all|ce|dm}     default: all

# Orchestrated startup:
--run-config <path>              # required; global parameters only (see spec-orchestrator §5.5)
--instance <name>                # required; logging/audit identifier, not a config lookup key
# plus per-instance flags (§9.3 in spec-orchestrator, table below)
```

Do not introduce `--ce-only` / `--dm-only` flags or the short alternatives `-C` / `-D`. This avoids two parallel CLI contracts.

When `--run-config` is specified:

1. **Global** functional flags that duplicate file contents (`--mode` without `--role`, `-c`, `-t`, `-f`, `-w`, `-i`, `-h`, `-p`, `--bh-endpoints`, `--mee-endpoints`, `--base-time-epoch`, and similar) are prohibited.
2. **Per-instance** flags are required or allowed as listed below and in [`spec-orchestrator.md`](spec-orchestrator.md) §9.3. There is no precedence rule between file and argv because they cover disjoint parameter sets.
3. For `Driver.exe`, `--role {ce|dm|standalone}` replaces `--mode` and selects the execution path; `--mode` without `--role` is not allowed with `--run-config`.

Mapping parameters to sources:

| Parameter | Source | Roles | Runtime semantics |
| --- | --- | --- | --- |
| `scale.*`, `paths.data` | run-config | Driver, MEE | manual `-c` / `-t` / `-f` / `-w` / `-i` |
| `scale.duration_sec` | run-config | CE | Measurement duration from manual `-d` |
| `database.*`, `client_side` | run-config | BH | DB connection and client-side path from manual `-h/-p/-d/-1` |
| `base_time_epoch` | run-config | MEE | scheduled `SetBaseTime`, manual `--base-time-epoch` |
| `endpoint_sets.bh` | run-config | CE, DM, MEE | outgoing BH pool, manual `--bh-endpoints` |
| `endpoint_sets.mee` | run-config | BH | outgoing MEE pool, manual `--mee-endpoints` |
| `--role` | argv | Driver | `ce` / `dm` / `standalone`; `standalone` ≡ manual `--mode all` |
| `--instance` | argv | all | instance name for logs and orchestrator state |
| `-l` / listen port flag | argv | BH, MEE | listen port |
| `-o` | argv | all | per-instance output directory |
| `--ready-file` | argv | BH, MEE | service-ready marker path |
| `--pool-init-timeout` | argv | all with outgoing pool | bootstrap timeout in seconds |
| `-U` / `--unique-id` | argv | MEE | MEE instance UniqueId |
| `-u`, `--ce-id-base`, partition flags | argv | CE, standalone | CE threads, global UniqueId interval, C_ID partition |
| `-d` | argv | DM, standalone | DM lifetime upper bound or standalone duration |

The runtime MUST read global fields from the file and per-instance fields from argv. Local defaults MUST NOT override run-config global fields. Per-instance argv MUST be present where required; missing flags are a startup error.

The file format is defined by [`spec-orchestrator.md`](spec-orchestrator.md) §5.5.

Canonical semantics of existing parameters: `-c` is the active customer count, and `-t` is the configured customer count. The `usage()` text MUST be aligned with the actual parsing behavior.

Inherited Driver parameters retain their semantics:

| Flag | Meaning |
| --- | --- |
| `-f` | scale factor |
| `-w` | initial trade days |
| `-d` | in `ce`/`all`, Measurement duration; in `dm`, the upper bound on the DM process lifetime |
| `-i` / `-o` | EGen input / a separate output directory for the process |
| `-h/-p` or `--bh-endpoints` | one BH or the full BH pool |

Under orchestration, `-d` for DM is calculated using the formula in [`spec-orchestrator.md`](spec-orchestrator.md) §9.3 and is not copied from the CE `-d`.

### 5.2. Driver / Customer: C_ID Partitioning

**Files:** `DriverMain.cpp`, `Customer.{h,cpp}`, and the `CDriver` signature if necessary.

**New CE-instance parameters:**

| Parameter | Meaning | Compliant default / constraint |
| --- | --- | --- |
| `iMyStartingCustomerId` | partition start | `% 1000 == 1` |
| `iMyCustomerCount` | partition size | `>= 5000`, `% 1000 == 0` |
| `iPartitionPercent` | proportion selected from the partition | `50` for a compliant run |

**CLI for manual startup:**

```text
--ce-start-id <TIdent>     # iMyStartingCustomerId
--ce-part-count <TIdent>   # iMyCustomerCount
--ce-part-percent <INT32>  # default 50; 0 + zero start/count = partitioning off
```

**Implementation:**

1. Partitioning is disabled only when none of the three flags are provided or when they are explicitly set to `(start_id, count, percent) = (0, 0, 0)`; use the current `CCE` constructor.
2. Partitioning is enabled only when all three parameters are provided and the tuple is nonzero; use the partitioned `CCE` constructor. A partially specified or partially zero tuple is a fatal validation error; silent fallback is prohibited.
3. Before startup, call `CDriverCEPartitionSettings::CheckValid()` / `CheckCompliant()` (or perform equivalent validation) and terminate immediately on a violation.
4. Write the partition settings to the Driver log (for FDR / audit).
5. `-c` / `-t` continue to specify the **global** configured/active customers in the database; the partition is a subset.

**UniqueId allocation across processes:**

```text
--ce-id-base <UINT32>   # base UniqueId for this process's CE threads
```

The thread at slot index `i` (`0..N-1`) receives `UniqueId = ce-id-base + i`; `ce-id-base >= 1`. A process with `N` threads occupies the half-open UniqueId interval `[ce-id-base, ce-id-base + N)`. These half-open intervals MUST NOT overlap between CE processes.

The thread slot index and `UniqueId` are distinct concepts. Arrays of `pthread_t` and other local arrays MUST be indexed only by slot index (`0..N-1`), not by global `UniqueId`; in particular, access such as `g_tid[UniqueId]` is invalid. This is a mandatory part of the `--ce-id-base` implementation.

`CDriverCEPartitionSettings::CheckValid()` / `CheckCompliant()` in the bundled EGen does not replace local validation of every constraint in §2.2. The runtime MUST separately enforce the minimum partition size of `5000`, even if the EGen version in use accepts a smaller value.

### 5.3. MarketExchange: Parameterized UniqueId and BaseTime

**Files:** `MarketExchangeMain.cpp`, and `MarketExchange.{h,cpp}` if necessary.

**CLI:**

```text
# Manual startup:
-U <UINT32>                    # MEE instance UniqueId (required for multi-MEE; default 1 for compatibility)
--base-time-epoch <unix_ts>    # shared scheduled epoch for all MEE instances

# Orchestrator:
--run-config <path> --instance <mee-name> -U <unique_id> -l <port> …
```

**UniqueId behavior:**

1. Pass `UniqueId` to `CMarketExchange` / `CMEE` instead of the constant `1`.
2. Print the UniqueId in the startup log.
3. At startup, the runtime MUST reject duplicate MEE UniqueId values when they would cause an RNG collision. Under orchestration, `tpcectl validate` checks profile uniqueness; each MEE passes its own `-U` on the command line. Manual multi-MEE startup requires the operator to provide distinct `-U` values.

**`SetBaseTime` synchronization:**

1. For multi-MEE, all processes receive one `base_time_epoch` from the shared run-config (or the same `--base-time-epoch` in a manual startup).
2. MEE opens its listen socket before the epoch but MUST NOT accept production workload until its outgoing BH pool is initialized and the epoch has arrived.
3. `SetBaseTime()` is called exactly once when the epoch arrives; calling it directly in the constructor before waiting for the epoch is prohibited.
4. The epoch MUST be sufficiently far in the future for bootstrap. For manual multi-MEE startup: `epoch >= now + pool_init_timeout + 5s`; the orchestrator uses the more conservative formula from [`spec-orchestrator.md`](spec-orchestrator.md) §9.4. If the epoch has already passed by the time full readiness is reached, MEE fails startup; it MUST NOT silently use the local current time.
5. For single-MEE, omitting the manual flag preserves legacy behavior with an immediate `SetBaseTime()`. The orchestrator always writes the epoch to run-config, including for a single MEE.

Barrier files, FIFOs, and UDP are not part of CLI v1. The only normative synchronization mechanism is a scheduled epoch.

### 5.4. Connection Pool to BH and MEE (Round-Robin per Operation)

**Files:** `CESUT.*`, `DMSUT.*`, `MEESUT.*`, `TxnHarnessSendToMarket.*`, a shared pool helper (a new module under `dbt5/common/`), and the Driver / BH / MEE CLI.

External load balancers MUST NOT be part of the target architecture. Distribution across BH and MEE instances is implemented **in the application** as a pool of pre-established TCP connections.

#### 5.4.1. Where Pools Are Required

| Client | Servers in pool | Operations |
| --- | --- | --- |
| CE / DM (`CESUT`, `DMSUT`) | `BH₁ … BHₖ` | send CE/DM transactions and receive responses |
| MEE (`MEESUT`) | `BH₁ … BHₖ` | Trade-Result, Market-Feed |
| BH (`TxnHarnessSendToMarket`) | `MEE₁ … MEEₘ` | `send_to_market` (Trade Request) |

One process maintains **one shared pool per target role** (for example, one CE process has one shared pool to BH), rather than a separate pool for every worker. The pool size equals the number of configured endpoints: exactly one slot per endpoint and at least one live connection in every slot after successful initialization. A slot handles no more than one operation at a time; its lock covers the entire send+receive RPC.

#### 5.4.2. Endpoint Configuration

```text
# Manual Driver/MarketExchange startup:
--bh-endpoints host1:30000,host2:30000,...

# Manual BrokerageHouse startup:
--mee-endpoints host1:30010,host2:30010,...

# Manual startup of any process with an outgoing pool:
--pool-init-timeout <seconds>   # >= 1; default 60 for manual startup
```

Under orchestrated startup, global endpoint lists and `base_time_epoch` come from the shared `run-config.json`. Per-instance listen port, output directory, ready file, pool timeout, UniqueId, role, CE partition, and DM/standalone duration come from argv (see [`spec-orchestrator.md`](spec-orchestrator.md) §9.3). The same global file is distributed to all participating hosts.

Manual flags remain available for standalone/legacy startup without `--run-config`. If a single `-h`/`-p` (or `-m`/`-M` for BH) is specified without `--*-endpoints`, the pool contains one endpoint. Mixing `--run-config` with duplicate **global** flags is prohibited.

#### 5.4.3. Pool Initialization (Test Initialization Phase)

BH and MEE have three readiness stages:

- **listen-ready** — the listen socket is bound and `listen()` has been called; a TCP `connect()` to the endpoint succeeds. The process has **not** yet entered its `accept()` loop; outgoing-pool initialization may still be in progress.
- **pool-ready** — all mandatory outgoing slots are connected and the reconnect thread is running;
- **service-ready** — role-specific barriers are also complete: for MEE, the shared epoch has arrived and `SetBaseTime()` has run; for BH, pool-ready is sufficient. The process atomically creates the `ready_file` and immediately enters its `accept()` loop.

**Per-process server startup (BH and MEE, current implementation):**

1. Open the listen socket (`dbt5Listen`).
2. Initialize the outgoing pool to the opposite role with bounded retries (`--pool-init-timeout`).
3. For MEE only: wait for `base_time_epoch` (if scheduled) and call `SetBaseTime()`.
4. Atomically create the `ready_file` from `--ready-file <path>` (orchestrated argv).
5. Enter the `accept()` loop and begin serving inbound connections.

Steps 2–4 run **before** step 5. Inbound bootstrap connections from the opposite role therefore cannot be actively accepted while the outgoing pool is initializing (or, for MEE, while waiting for the epoch). Clients that connect during this window rely on the kernel TCP listen backlog to queue the connection until step 5 begins (`LISTENQ = 1024` in `dbt5/common/CSocket.cpp`). This deferred-accept model is **not** equivalent to accepting and holding bootstrap connections during listen-ready; orchestrator timeouts and pool-init retry policies must account for it.

A stale ready file is removed before startup begins. For a role without an outgoing pool or a role-specific barrier, service-ready still follows pool-ready and precedes the `accept()` loop. A successful TCP connect verifies only listen-ready (step 1 complete) and does not indicate full readiness. Production transaction workload is not processed before service-ready.

Before transaction workload begins (before DM Trade-Cleanup / ramp-up / Measurement Interval):

1. Read the endpoint list.
2. Establish a TCP connection to **every** endpoint.
3. During bootstrap, retry connections with bounded backoff until `--pool-init-timeout` (orchestrated argv, or the same flag for manual startup), measured from process start. Failing immediately on the first unavailable peer is prohibited because the opposite role may still be between steps 1 and 5 above. When the timeout expires, the process removes its ready file, logs the unconnected endpoints, and exits with an error.
4. Fix the connection ring and initialize an atomic/mutex-protected round-robin index to 0.
5. Start a **background recovery thread** (§5.4.5).
6. Complete the role-specific barrier; MEE waits for the epoch and calls `SetBaseTime()`.
7. Atomically create the ready file, enter the `accept()` loop, and transition to service-ready.
8. Start DM only after all BH/MEE instances are service-ready, and start CE only after Trade-Cleanup completes.

The orchestrator passes `--pool-init-timeout` of `2 * timeouts.ready` seconds to BH processes because BH starts before MEE; MEE, DM, and CE receive `timeouts.ready` (see [`spec-orchestrator.md`](spec-orchestrator.md) §9.3). If BH reaches the timeout before service-ready, or DM/CE does not initialize the complete BH pool before its first transaction, the process exits with a nonzero code, and the orchestrator marks the run as failed and performs the normative stop procedure.

The pool does **not** create connections on demand in the operation hot path, except for the retry-after-disconnection scenario (§5.4.4); even there, using an already recovered connection from the pool is preferred.

#### 5.4.4. Selection Policy and Error Behavior

For **each individual operation** (one RPC: sending a request and waiting for the response, or one `send_to_market`):

1. Select the next connection by **round-robin** (slot index `(i+1) mod N` in the pool).
2. Perform the operation on the selected connection.
3. Slot selection MUST NOT depend on message contents (transaction type, C_ID, symbol, etc.), as required by 4.4.1.3.

If the selected connection **breaks or encounters an I/O error**:

1. Mark the slot as broken (close the connection and remove it from RR until recovery).
2. Automatic retry on the next live connection is allowed only if the transport can guarantee that the server received no bytes from the operation (for example, a connection failure before send).
3. After a partial/full send or a receive error, the outcome of the operation is unknown. Until the protocol provides an operation ID and deduplication, retrying such a non-idempotent operation is prohibited: the operation fails and the run is marked as failed.
4. For an allowed retry, continue round-robin selection until the operation succeeds or all currently alive slots have been exhausted in one cycle.
5. If no live connections remain, the operation fails. During the Measurement Interval, any such error makes the run unsuccessful.

Response Time accounting: the `sTn`…`eTn` interval includes retries on the next connection (measurement remains at the Driver). Response Time measurement MUST NOT rely on masking by an external load balancer.

#### 5.4.5. Background Recovery of Broken Connections

A dedicated thread (one per pool):

1. Periodically (or when signaled by the hot path) scans slots with broken status.
2. Attempts to `connect` again to the corresponding `host:port`.
3. On success, returns the slot to the RR ring (alive status).
4. On failure, applies backoff (for example, exponential with a cap) without blocking worker threads.
5. Does not execute transaction logic or read transaction payloads; it handles transport only.

Implementation requirements:

- thread safety for the RR index and slot statuses;
- the hot path MUST NOT wait for reconnect for an extended period (reconnect runs only in the background; the hot path may only retry on already alive peers);
- orderly termination of the reconnect thread during process shutdown.

#### 5.4.6. Prohibited Approaches

- Do not introduce an external L4/L7 VIP / DNS RR as a replacement for the pool.
- Do not select an endpoint based on fields in `TMsgDriverBrokerage` / `TTradeRequest`.
- Do not use a sticky “one connection for the entire Test Run” policy as the only scaling strategy: sticky behavior is allowed only as the degenerate case of a pool of size 1.

### 5.5. BrokerageHouse: Readiness for a Pool of Instances

No major changes to transaction logic are required (client-side mode `-1` is already stateless with respect to local state). The following operational changes are required:

1. **Startup/health:** print the listen address/port from argv and create the `ready_file` from `--ready-file`; support the same flag for manual startup. The file indicates service-ready, not merely an open listen socket.
2. **Worker limit (optional):** `--max-workers <N>` — reject incoming connections above the limit instead of using unbounded `pthread_create` (protection against resource exhaustion; not a TPC-E requirement).
3. **Logs:** use a unique `outputDirectory` per instance (`-o`) to avoid overwriting files when multiple BH instances run on one host.
4. **Startup documentation:** document orchestrated mode through the shared global run-config plus per-instance argv, and separately document manual mode with multiple BH instances on different `-l` values and the MEE list passed through `--mee-endpoints`.

Do **not** implement BH sharding by C_ID range.

### 5.6. Orchestration and Startup Examples

**Add** (under `docs/` or `scripts/`, as appropriate for the implementation):

1. An example configuration for N CE / 1 DM / M MEE / K BH.
2. An example partition layout for common database sizes (5k, 50k, 100k customers).
3. A script or make target named “smoke scale” that starts 2 CE + 1 DM + 2 MEE + 2 BH against one database and runs for a short duration.
4. A checklist of compliant parameters before measurement.

**Example startup after distributing the shared global run-config:**

```bash
RUN_CONFIG=/opt/tpce/runs/20260717T100000Z/run-config.json

BrokerageHouseMain-pgsql.exe --run-config "$RUN_CONFIG" --instance bh1 \
  -l 30000 -o /opt/tpce/runs/20260717T100000Z/bh1 \
  --ready-file /opt/tpce/runs/20260717T100000Z/bh1/.service-ready \
  --pool-init-timeout 120
BrokerageHouseMain-pgsql.exe --run-config "$RUN_CONFIG" --instance bh2 \
  -l 30000 -o /opt/tpce/runs/20260717T100000Z/bh2 \
  --ready-file /opt/tpce/runs/20260717T100000Z/bh2/.service-ready \
  --pool-init-timeout 120
MarketExchange.exe --run-config "$RUN_CONFIG" --instance mee1 \
  -l 30010 -U 1 -o /opt/tpce/runs/20260717T100000Z/mee1 \
  --ready-file /opt/tpce/runs/20260717T100000Z/mee1/.service-ready \
  --pool-init-timeout 60
MarketExchange.exe --run-config "$RUN_CONFIG" --instance mee2 \
  -l 30010 -U 2 -o /opt/tpce/runs/20260717T100000Z/mee2 \
  --ready-file /opt/tpce/runs/20260717T100000Z/mee2/.service-ready \
  --pool-init-timeout 60
Driver.exe --run-config "$RUN_CONFIG" --role dm --instance dm0 \
  -o /opt/tpce/runs/20260717T100000Z/dm0 --pool-init-timeout 60 -d 9340
Driver.exe --run-config "$RUN_CONFIG" --role ce --instance ce1 \
  -u 64 --ce-id-base 1000 -o /opt/tpce/runs/20260717T100000Z/ce1 \
  --pool-init-timeout 60 \
  --ce-start-id 1 --ce-part-count 25000 --ce-part-percent 50
Driver.exe --run-config "$RUN_CONFIG" --role ce --instance ce2 \
  -u 64 --ce-id-base 2000 -o /opt/tpce/runs/20260717T100000Z/ce2 \
  --pool-init-timeout 60 \
  --ce-start-id 25001 --ce-part-count 25000 --ce-part-percent 50
```

The order and readiness barriers remain normative as specified in §3; these commands illustrate argv form only, not a sequential shell script.

### 5.7. Metrics and Log Collection

For a multi-instance deployment:

1. Each CE/MEE/DM has its own `-o` directory.
2. A tool (script) is required to aggregate mix logs / RT for tpsE calculation and mix verification (this may be deferred to a separate task, but the log format MUST NOT be broken).
3. For partitioned CE, preserve the ability to export Trade-Results per Load Unit per minute for EGenTester (Clause 6.6.3).

---

## 6. Acceptance Criteria

The changes are complete when:

| ID | Criterion |
| --- | --- |
| A1 | One run-config can start ≥2 CE instances without a second DM |
| A2 | Exactly one instance with role `dm` performs Trade-Cleanup and the once-per-minute DM operation |
| A3 | CE accepts partition parameters; the runtime rejects every violation of §2.2, including a size `<5000`, regardless of gaps in EGen's `CheckCompliant` |
| A4 | A partitioned CE uses the partitioned `CCE` constructor (not the full non-partitioned path) |
| A5 | ≥2 MEE instances with distinct `unique_id` values from argv start and process trade requests |
| A6 | All MEE instances using a shared `base_time_epoch` from run-config call `SetBaseTime()` exactly once and no earlier than the epoch; a past epoch is rejected |
| A7 | The CE/DM/MEE→BH and BH→MEE connection pools implement deferred-accept bootstrap (listen socket before outgoing pool and `accept()` loop; inbound connections queued in TCP backlog until service-ready), service-ready before workload, per-operation RR, safe retry only before bytes are sent, and background reconnect, without selection based on packet contents |
| A8 | CE UniqueIds do not overlap between processes (`--ce-id-base`) |
| A9 | A short multi-instance smoke run completes without transaction errors |
| A10 | All processes accept a byte-identical global run-config and start with `--run-config` plus the per-instance argv from spec-orchestrator §9.3; the README or this document contains an example and a list of prohibited configurations |

---

## 7. Prohibited Configurations (Do Not Implement / Reject in Documentation)

1. Multiple processes with an active DM.
2. C_ID partitioning with `iPartitionPercent ≠ 50` in a declared compliant run.
3. A partition start/size that is not aligned to a Load Unit boundary / is &lt; 5000.
4. Multiple MEE instances with the same `UniqueId`.
5. A CE→BH or BH→MEE router that reads transaction type / C_ID / symbol from the message, or an external L4/L7 load balancer used instead of the application's connection pool.
6. Different BH instances connected to different database “shards” partitioned by customer (application-level sharding).
7. Moving Frame Implementation into stored procedures as a replacement for Tier-A scaling within *this* variant (the client-side variant MUST remain available; do not remove the server-side path `-` without `-1`, but do not conflate the goals).

---

## 8. Implementation Order (Recommended)

1. **P0 — configuration infrastructure:** a shared versioned global `run-config.json` parser for Driver/BH/MEE, plus per-instance argv parsing (`--instance`, `--role`, listen/output/ready/pool flags); then parsing of manual long options without `--run-config`; correct the `-c`/`-t` usage text; add mode-aware validation.
2. **P0 — multi-Driver correctness:** `--role ce|dm|standalone` from argv with `--run-config`, then `ce_id_base` together with separation of slot index from global UniqueId; retain manual `--mode` for compatibility.
3. **P0 — MEE identity/time:** `-U` and `base_time_epoch` from argv/file respectively, remove the hard-coded `1`, and implement delayed `SetBaseTime()`; retain manual `-U`/`--base-time-epoch` for compatibility.
4. **P1 — transport layer:** a shared process-level pool to BH/MEE, deferred-accept bootstrap (§5.4.3), argv-driven ready file, per-operation RR, safe retry policy, and background reconnect (§5.4).
5. **P1 — CE partitioning:** CLI + partitioned `CCE` + complete local validation, including the minimum of 5000.
6. **P2 — operations:** BH worker limits / DB connection sizing, startup examples, and smoke script.
7. **P2 — orchestration and reporting:** a complete multi-instance smoke test after items 1–5; log aggregation / EGenTester export.

Every P0/P1 item MUST include a brief section in each affected binary's usage output and an updated link from the root `README.md` to this document.

---

## 9. Conformance with the Client-Side Model

The variant without stored procedures means:

- all Frame Implementation remains in `CDBConnectionClientSide` / `*DB` within Tier-A;
- horizontal scaling of SQL-logic CPU means **more BH processes**, not enlarging one BH process or moving logic to Tier-B;
- the number of DB sessions ≈ the number of concurrent worker connections across all BH instances; PostgreSQL-side connection pools MUST be sized for `sum(BH workers)`.

This is an expected and acceptable consequence of the model; the constraints in §2 remain fully applicable.

---

## 10. References

- TPC-E v1.14.0: Clause 4 (Driver/Tier-A/Tier-B configuration, network, no-peeking), Clause 6.3.2 (CE partitioning), and the CE/MEE/DM definitions.
- EGen: `egen/egenlib/CE.h`, `MEE.h`, `DriverParamSettings.h` (`CDriverCEPartitionSettings`).
- Runtime: `dbt5/common/StartupCli.cpp`, `RunConfig.cpp`, `EndpointConnectionPool.cpp`, `Driver.cpp`, `Customer.cpp`, `MarketExchangeMain.cpp`.
- Database schema: `sql/pgsql/README.md`.
