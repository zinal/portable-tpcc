# Specification: TPC-E Testbed Orchestrator (`tpcectl`)

Implementation language: **Go** (a single statically linked or otherwise self-contained binary).
Out of scope: Ansible, Kubernetes, and systemd as a required runtime (they may be added later as optional features).

Runtime assumption: the `dbt5` binaries have been extended as specified in [`spec-scalability.md`](spec-scalability.md): they read a shared `run-config.json` for **global** test parameters, receive **per-instance** settings through command-line flags (`--instance`, `--role`, `--listen`, and so on), and implement Driver modes, C_ID partitioning, multi-MEE UniqueId/BaseTime handling, and endpoint pools. The orchestrator does **not** implement these capabilities itself; it generates the shared configuration once per run and starts the extended programs with role-specific argv.

---

## 1. Purpose

`tpcectl` is a lightweight orchestrator for running a fair-use TPC-E testbed from this repository:

- distributing artifacts to hosts (**deploy**);
- preparing and loading the database (**schema**, **load**);
- starting and stopping **BH / MEE / DM / CE** through **SSH + nohup** in a compliant multi-instance topology;
- coordinating **BaseTime** across multiple MEE instances and centralizing a **single DM**;
- collecting results (**collect**);
- removing deployment artifacts (**cleanup**).

The orchestrator does **not** reimplement benchmark logic, use upstream DBT-5 shell scripts, or compile testbed programs. It copies files and manages the lifecycle from a declarative profile.

---

## 2. Explicit Non-Goals

- It is not Ansible, Terraform, or a Kubernetes operator.
- It is not a replacement for PostgreSQL administration. Creating the cluster and tuning `postgresql.conf` are outside the orchestrator. Tables and partitions are created by applying SQL from `sql/pgsql/` directly; an external `psql` in PATH is not required when the Go driver is sufficient, but `psql` is allowed as a fallback.
- It is not an interactive TUI.
- Server-side stored procedures are not supported as a target mode. The testbed uses client-side mode, `BrokerageHouseMain -1`.
- It does not audit or certify official TPC results, but its topology and argv must **permit** a compliant run under [`spec-scalability.md`](spec-scalability.md) §2.
- It is not an external L4/L7 load balancer. CE/DM/MEE↔BH and BH↔MEE distribution uses only **application connection pools** with round-robin selection.

---

## 3. Design Principles

1. **One binary**, `tpcectl`, written in Go; embedded SSH (`golang.org/x/crypto/ssh` + SFTP) is preferred.
2. **Declarative profile** in YAML: topology, scale, CE partitions, endpoint pools, and artifacts.
3. **Idempotency where possible**: repeated `deploy` updates files; `cleanup` uses a deployment manifest.
4. **Transparency**: `plan` prints actions and argv without side effects.
5. **Topology defined by [`spec-scalability.md`](spec-scalability.md):**
   `BH₁…BHₖ` + `MEE₁…MEEₘ` + **exactly one** `DM` + `CE₁…CEₙ` → **one** database.
   These are flat role lists with a shared endpoint pool, not “Driver+BH+MEE groups.”
6. Process management uses **nohup**, PID files, and logs in the run directory.
7. The orchestrator provides the **centralized** aspects of a Test Run: one DM, one Trade-Cleanup through the DM, one shared BaseTime epoch, uniform scale/EGen parameters, and synchronized start/stop/collect operations.
8. All UniqueId intervals are half-open intervals `[start, end)`. Every timeout is a maximum wait time; when it expires, the command exits with a nonzero status and stops any processes it already started.
9. Desired configuration and observed state are separate: one immutable `run-config.json` with **global** parameters is generated once per run and distributed byte-identically to every runtime host; **per-instance** parameters (listen port, role, output directory, UniqueId, CE partition, and so on) are passed only through argv. Mutable `run-state.json` is stored only locally on the control host. Runtime hosts never receive a copy of the orchestrator's shared state.

---

## 4. Roles and Topology

### 4.1. Logical Roles

| Role | Processes | Notes |
| --- | --- | --- |
| `control` | host running `tpcectl` | may also be one of the other hosts |
| `db` | PostgreSQL (Tier-B) | the orchestrator does not start PostgreSQL |
| `loader` | `Loader-pgsql.exe` (1..N shards) | load phase |
| `bh` | `BrokerageHouseMain-pgsql.exe` (1..K) | Tier-A; client-side `-1` |
| `mee` | `MarketExchange.exe --run-config … --instance <name> -U <id> …` (1..M) | part of the Driver; unique `unique_id` on the command line |
| `dm` | `Driver.exe --run-config … --role dm --instance <name> …` | **exactly one** instance with role `dm` |
| `ce` | `Driver.exe --run-config … --role ce --instance <name> …` (1..N) | role `ce` on the command line, optionally with a C_ID partition |
| `results` | collect directory | often on the control host |

The `standalone` instance role is allowed only for a **single-process** smoke/legacy run, where one Driver provides CE+DM. In a multi-instance profile, `validate` rejects more than one process with an active DM role (`standalone` or `dm`).

### 4.2. Target Topology (Normative)

```text
DRIVER (outside the SUT)
┌────────────────────────────────────────────────────────────┐
│  CE₁ … CEₙ   Driver.exe --run-config … --role ce --instance ceN … │
│  DM          Driver.exe --run-config … --role dm --instance dm0 …   │
│  MEE₁ … MEEₘ MarketExchange.exe --run-config … --instance meeM …    │
│  conn pool → BH₁…BHₖ  (endpoint_sets.bh from run-config, RR)        │
└───────────────┬────────────────────────────▲───────────────┘
                │ CE/DM → BH                 │ TR/MF ← MEE
                ▼                            │
TIER-A (SUT)
┌────────────────────────────────────────────────────────────┐
│  BH₁ … BHₖ   BrokerageHouseMain --run-config … --instance bhK …     │
│  conn pool → MEE₁…MEEₘ  (endpoint_sets.mee from run-config, RR)   │
└────────────────────────────┬───────────────────────────────┘
                             ▼
TIER-B — one logical PostgreSQL database
```

Process-to-endpoint relationships:

| Client | Servers | Configuration |
| --- | --- | --- |
| every CE, the DM, and every MEE | all BH instances | `endpoint_sets.bh` from `run-config.json` |
| every BH | all MEE instances | `endpoint_sets.mee` from `run-config.json` |

Manual `--bh-endpoints` / `--mee-endpoints` and single `-h`/`-p` / `-m`/`-M` options are retained only for operation without the orchestrator. `tpcectl` does not generate them.

### 4.3. Start and Stop Sequence

**Start (`run` / `start`):**

1. Confirm that the database is reachable, optionally by pinging it.
2. Select `run_id` and the shared `base_time_epoch`, then generate immutable `run-config.json`. Create local `run-state.json` with state `starting`. Within `timeouts.config_distribute`, atomically distribute the same run-config to all participating hosts and verify the SHA-256 of every copy.
3. Start all **BH** instances in parallel. Within `timeouts.ready` from BH startup, wait until every BH is listen-ready by opening a TCP connection to each listening endpoint. During this period, each BH initializes its outgoing pool to the not-yet-started MEE instances with bounded retries.
4. Immediately after the last BH becomes listen-ready, start all **MEE** instances in parallel. Within `timeouts.ready` from MEE startup, wait until every MEE is listen-ready.
5. Wait for the service-ready files from all MEE and BH instances. For an MEE, service-ready includes pool-ready, arrival of the shared epoch, and a completed `SetBaseTime()` call; for a BH, it includes pool-ready. A listening TCP socket alone does not indicate full readiness. A BH receives a pool initialization timeout of `2 * timeouts.ready`; other roles receive `timeouts.ready`. The overall service-ready deadline is `timeouts.config_distribute + 2 * timeouts.ready + base_time_lead_sec + 5s` from the beginning of step 2.
6. Start **one DM** and wait for the stable Trade-Cleanup completion marker described in §9.5. DM process termination is not a Trade-Cleanup completion marker.
7. Start all **CE** instances in parallel; ramp-up occurs inside the Driver.
8. Atomically change local `run-state.json` to state `running`.

**`run` after CE startup:**

Here and below, effective `duration_sec` means `standalone_driver.duration_sec` when `standalone_driver.enabled` is true, and `scale.duration_sec` otherwise.

1. Do not use `sleep(duration_sec)` as the indication that the run has ended. Wait for **all** CE instances to terminate naturally; each Driver accounts for its own ramp-up and effective `duration_sec`.
2. The wait deadline from CE startup is effective `duration_sec + timeouts.ce_completion_grace`.
3. A nonzero exit from any CE, disappearance of a process without an exit status, or expiry of the deadline makes the run failed; perform the normative stop and collect sequence.
4. If a CE exits before its effective `duration_sec` has elapsed since its startup, treat this as an early exit and mark the run failed, even if its exit code is 0.

**After all CE instances complete successfully, or on `stop`:**

1. Send SIGTERM to any **CE** instances that are still running and wait up to `timeouts.stop_grace` for them to exit.
2. Keep DM/MEE/BH running for the fixed `timeouts.mee_drain` interval so in-flight Trade-Result and Market-Feed operations can complete. Do not start new CE instances.
3. Stop the **DM**, then the **MEE** instances, then the **BH** instances, waiting for each role to terminate before proceeding to the next role.
4. Run `collect`, unless `--skip-collect` is set.

The `start` command performs the same steps 1–8, including the Trade-Cleanup barrier and CE startup, but returns after updating local `run-state.json` without waiting for the CE instances. `stop` and Ctrl+C always use the sequence above.

For `standalone_driver`, a single Driver instance with role `standalone` replaces the separate DM and CE steps and starts after BH/MEE become service-ready. The orchestrator waits for the Trade-Cleanup completion marker from this process, then treats it as both the only DM and the only CE. `run` then waits for its natural termination under the same deadline and exit rules. `T0` for the deadline and early-exit check is the time at which the Trade-Cleanup completion marker is received, which is the beginning of CE ramp-up; Trade-Cleanup time is not included in effective `duration_sec`.

---

## 5. Configuration

### 5.1. Format

- YAML with `apiVersion: tpcectl/v1`.
- Database secrets are provided through the environment or a mode 0600 file and must **not** be committed.

Templating in v1 is not arbitrary Go templating. Only the exact placeholders `{{ run_id }}`, `{{ local_bin }}`, `{{ local_data }}`, and `{{ local_sql }}` from profile fields are supported. An unknown placeholder, template action, function, or unclosed construct is a validation error. Expansion occurs once after `run_id` is selected, without shell evaluation.

### 5.2. SSH

```yaml
ssh:
  user: ""              # empty → $USER
  port: 22
  private_key: ""       # empty → ~/.ssh/id_ed25519, id_rsa, …
  known_hosts: ""       # empty → ~/.ssh/known_hosts; strict by default
  connect_timeout: 15s
  # password authentication is not supported in v1
```

A per-host override at `hosts.<name>.ssh` is allowed.

### 5.3. Multi-Instance Profile Example

```yaml
apiVersion: tpcectl/v1
kind: Profile

name: lab-50k-scale
run_id: ""                 # empty → timestamp/UUID when run starts

ssh:
  user: ""
  private_key: ""

paths:
  local_bin: ./build-out
  local_data: ./data
  local_sql: ./sql/pgsql
  remote_root: /opt/tpce

scale:
  customers: 50000           # configured = active in a typical lab; divisible by 1000
  active_customers: 50000    # defaults to customers when omitted
  scale_factor: 500
  initial_trade_days: 300
  duration_sec: 7200
  client_side: true          # always -1 for BH

# Shared epoch for all MEE instances, in Unix seconds. When empty at run time →
# now + config_distribute + 2*ready + base_time_lead_sec.
base_time_epoch: null
base_time_lead_sec: 30

timeouts:
  config_distribute: 30s # distribute and verify run-config on every runtime host
  ready: 60s
  cleanup_wait: 300s         # wait for Trade-Cleanup from the DM
  ce_completion_grace: 30m   # upper bound for CE ramp-up/completion beyond -d
  mee_drain: 10s             # fixed drain after CE stop/exit
  stop_grace: 30s

db:
  host: db1.example
  port: 5432
  name: tpce
  user: tpce
  password_env: TPCE_PGPASSWORD
  sslmode: prefer

schema:
  mode: partitioned          # base | partitioned
  partitions: 32
  apply_indexes: true
  apply_fks: false

hosts:
  db1:  { address: 10.0.0.10 }
  mid1: { address: 10.0.0.11 }
  mid2: { address: 10.0.0.12 }
  load1: { address: 10.0.0.21 }
  load2: { address: 10.0.0.22 }
  drv1: { address: 10.0.0.31 }
  drv2: { address: 10.0.0.32 }
  drv3: { address: 10.0.0.33 }

deploy:
  artifacts:
    - { src: "{{ local_bin }}/BrokerageHouseMain-pgsql.exe", dst: bin/BrokerageHouseMain-pgsql.exe, mode: "0755" }
    - { src: "{{ local_bin }}/MarketExchange.exe", dst: bin/MarketExchange.exe, mode: "0755" }
    - { src: "{{ local_bin }}/Driver.exe", dst: bin/Driver.exe, mode: "0755" }
    - { src: "{{ local_bin }}/Loader.exe", dst: bin/Loader.exe, mode: "0755" }
    - { src: "{{ local_data }}", dst: data, recursive: true }
    - { src: /etc/tpce/certs/ca.pem, dst: certs/ca.pem, mode: "0644", optional: true }

load:
  shards:
    - { host: load1, begin: 1, count: 25000 }
    - { host: load2, begin: 25001, count: 25000 }

# --- Tier-A ---
bh:
  - name: bh1
    host: mid1
    listen: 30000
    output: runs/{{ run_id }}/bh1
  - name: bh2
    host: mid2
    listen: 30000
    output: runs/{{ run_id }}/bh2

# --- Driver: MEE ---
mee:
  - name: mee1
    host: mid1
    listen: 30010
    unique_id: 1
    output: runs/{{ run_id }}/mee1
  - name: mee2
    host: mid2
    listen: 30010
    unique_id: 2
    output: runs/{{ run_id }}/mee2

# --- Driver: exactly one DM ---
dm:
  name: dm0
  host: drv1
  output: runs/{{ run_id }}/dm0
  # The orchestrator calculates -d so the DM is guaranteed to outlive cleanup,
  # all CE instances, and drain; users cannot set a separate DM duration.

# --- Driver: CE (one without a partition by default; multiple with partitions) ---
ce:
  - name: ce1
    host: drv2
    users: 64
    ce_id_base: 1000
    partition:
      start_id: 1
      count: 25000
      percent: 50          # compliant value is always 50
    output: runs/{{ run_id }}/ce1
  - name: ce2
    host: drv3
    users: 64
    ce_id_base: 2000
    partition:
      start_id: 25001
      count: 25000
      percent: 50
    output: runs/{{ run_id }}/ce2

collect:
  dest: ./results/{{ run_id }}
```

The orchestrator constructs the following values:

- `bh_endpoints` = `mid1:30000,mid2:30000` (address:listen for every `bh`);
- `mee_endpoints` = `mid1:30010,mid2:30010`;
- it verifies that `mee.unique_id` values are unique, that `[ce_id_base, ce_id_base+users)` half-open intervals do not overlap, and that partition rules are satisfied.

### 5.4. Simplified Profile (Single-Instance / Smoke)

The degenerate case is allowed: one BH, one MEE, and one CE without a partition, with the DM combined with the CE through `standalone_driver.enabled: true` (`--role standalone` on the command line) **only when** the profile has no separate `ce[]` or `dm` entries.

```yaml
# Alternative for a lab smoke run: one Driver instance with role=standalone
standalone_driver:
  enabled: true
  host: drv1
  users: 8
  ce_id_base: 1
  duration_sec: 120
  output: runs/{{ run_id }}/driver_all
bh:
  - { name: bh1, host: mid1, listen: 30000, output: runs/{{ run_id }}/bh1 }
mee:
  - { name: mee1, host: mid1, listen: 30010, unique_id: 1, output: runs/{{ run_id }}/mee1 }
# dm: / ce: are not specified
```

`validate`: `standalone_driver.enabled` is mutually exclusive with nonempty `ce` or `dm`.

### 5.5. Runtime run-config (global only)

Before starting the first process, the orchestrator generates **one** shared immutable JSON file per run:

```text
<remote_root>/runs/<run_id>/run-config.json
```

Participating runtime hosts are the unique union of hosts from `bh`, `mee`, `dm`, `ce`, and `standalone_driver`. The `db`, `loader`, and control hosts are excluded unless they also run one of the runtime roles. The file is **byte-identical** on all participating runtime hosts and contains **only global** desired configuration — parameters that are the same for every process in the run. It does **not** contain per-instance entries, PIDs, lifecycle state, or readiness-check results:

```json
{
  "schema_version": 1,
  "run_id": "20260717T100000Z",
  "profile_name": "lab-50k-scale",
  "generated_at": "2026-07-17T10:00:00Z",
  "paths": {
    "data": "/opt/tpce/data"
  },
  "scale": {
    "active_customers": 50000,
    "configured_customers": 50000,
    "scale_factor": 500,
    "initial_trade_days": 300,
    "duration_sec": 7200
  },
  "database": {
    "host": "db1.example",
    "port": 5432,
    "name": "tpce",
    "user": "tpce",
    "sslmode": "prefer",
    "password_env": "TPCE_PGPASSWORD"
  },
  "client_side": true,
  "base_time_epoch": 1784282520,
  "endpoint_sets": {
    "bh": ["10.0.0.11:30000", "10.0.0.12:30000"],
    "mee": ["10.0.0.11:30010", "10.0.0.12:30010"]
  }
}
```

**Per-instance** parameters are **not** stored in this file. They are passed on the command line for each process (§9.3). This split lets the orchestrator distribute one small shared file and vary topology details only in argv.

Normative rules:

1. The orchestrator fully expands placeholders and writes absolute paths in the global sections. The runtime does not read the YAML profile or perform template expansion.
2. `--run-config <path>` is required for orchestrated startup. `--instance <name>` is required for every role and is used for logging, audit, and orchestrator state; it is **not** a lookup key into the JSON file.
3. `endpoint_sets` are complete address lists. Each process selects the correct set by role (`bh` endpoints for CE/DM/MEE outgoing pools; `mee` endpoints for BH). The runtime does not select endpoints based on the local hostname or modify the lists.
4. A `schema_version` unknown to the runtime, an unknown top-level field with the wrong type, or a missing required global field is a startup error. Schema v1 is strict; forward compatibility is introduced through a new supported version, not by silently ignoring data.
5. The file contains no passwords, private keys, PIDs, state, lifecycle-transition timestamps, or per-instance sections. `password_env` is only a variable name; its value is supplied to the process separately through a protected environment.
6. Before distribution, the orchestrator serializes the JSON deterministically, calculates its SHA-256, and stores the hash in local run-state. On each host, it writes `run-config.json.tmp`, performs an atomic rename, and then verifies the hash through SSH.
7. The run-config does not change after the first process starts. Any change to topology, scale, or BaseTime requires a new `run_id`.
8. When `--run-config` is specified, **global** functional flags that duplicate file contents (`--mode` without `--role`, scale, database, endpoints, BaseTime, EGen input path, and similar) are prohibited. **Per-instance** flags listed in §9.3 are required or allowed as specified there. There is no precedence rule between file and argv because they cover disjoint parameter sets.

Required global fields: `schema_version`, `run_id`, `paths.data`, full `scale`, full `database` (except the password value), `client_side`, `base_time_epoch`, and both `endpoint_sets` entries. Numeric constraints and cross-instance checks (UniqueId uniqueness, partition layout, and so on) are enforced by `tpcectl validate` and by each process at startup from its argv.

Runtime mapping of global fields is normative under `spec-scalability.md` §5.1: CE reads Measurement duration from `scale.duration_sec`; DM and standalone read per-instance `duration_sec` from argv; Driver/MEE read scale and `paths.data`; BH reads `database` and `client_side`. Role `standalone` on the command line denotes the same CE+DM+Trade-Cleanup path as manual `Driver --mode all`.

---

## 6. CLI

```text
tpcectl <command> -f PROFILE.yaml [flags]
```

### 6.1. v1 Commands

| Command | Description |
| --- | --- |
| `validate` | Validate the profile: divisibility by 1000, hosts, ports, exactly one DM or standalone, partition rules, MEE UniqueId values, ce_id_base values, and the sum of load shards |
| `plan` | Print a plan without executing it (`--for deploy\|schema\|load\|run\|…`) |
| `deploy` | Create `remote_root`, copy artifacts, and write the deployment manifest |
| `schema` | Apply DDL from `sql/pgsql/` |
| `load` | Run Loader shards in parallel |
| `run` | Perform the complete §4.3 lifecycle: wait for every CE to exit, stop, and collect |
| `start` | Perform the same steps 1–8, including CE startup; return immediately after state becomes `running`, without waiting for CE exit |
| `stop` | Stop the current run using PID files |
| `status` | Report whether PIDs are alive and ports are listening |
| `collect` | Download output directories |
| `cleanup` | Delete entries from the deployment manifest |
| `smoke` | Run a short test by overriding duration and users |

### 6.2. Global Flags

```text
-f, --file PROFILE.yaml
-v, --verbose
--dry-run
--run-id ID
--state-dir PATH                    # default: $XDG_STATE_HOME/tpcectl or ~/.local/state/tpcectl
--only-role bh|mee|dm|ce|loader   # optionally limit operations
```

The `--only-group` flag has been **removed** because the group model no longer exists.

### 6.3. `run` Flags

```text
--skip-deploy
--skip-schema
--skip-load
--skip-collect
--no-stop
--base-time-epoch UNIX   # override the profile
```

Happy path:

```bash
tpcectl validate -f profiles/lab-50k-scale.yaml
tpcectl deploy   -f profiles/lab-50k-scale.yaml
tpcectl schema   -f profiles/lab-50k-scale.yaml
tpcectl load     -f profiles/lab-50k-scale.yaml
tpcectl run      -f profiles/lab-50k-scale.yaml
tpcectl cleanup  -f profiles/lab-50k-scale.yaml --yes
```

---

## 7. Remote Layout (`remote_root`)

```text
/opt/tpce/
├── .tpcectl/
│   └── deploy-manifest.json       # host-local deployment journal, not run-state
├── bin/
├── data/
├── sql/                    # optional
├── certs/
├── runs/
│   └── <run_id>/
│       ├── run-config.json        # immutable, same SHA-256 on every host
│       ├── bh1/
│       ├── bh2/
│       ├── mee1/
│       ├── mee2/
│       ├── dm0/
│       ├── ce1/
│       └── ce2/
└── tmp/
```

Every instance has its **own** `-o` / `output`, as required for multi-instance operation by spec-scalability §5.5 / §5.7.

There is no shared run status on runtime hosts. Canonical mutable state is stored only on the control host:

```text
<state-dir>/
├── runs/<run_id>/run-state.json
└── profiles/<profile-id>/current-run.json
```

`current-run.json` is an atomically updated local index `{ "run_id": "..." }`, not a copy of state. `profile-id` is a safe name derived from profile `name` plus a short hash of the profile's absolute path, preventing conflicts between different files with the same name.

---

## 8. Deploy

Deploy semantics:

1. Run `validate`.
2. Hosts = union(`bh`, `mee`, `dm`, `ce`, `load.shards`, `standalone_driver`).
3. Create directories, copy files, apply modes, and write `deploy-manifest.json`.
4. The minimum artifact set is the four `*.exe` files plus `data/`, with optional certificates and SQL.

---

## 9. Starting Processes with nohup

### 9.1. Template

```bash
mkdir -p '<output_dir>'
cd '<remote_root>'
nohup '<bin>' <args> \
  > '<output_dir>/stdout.log' \
  2> '<output_dir>/stderr.log' \
  < /dev/null &
echo $! > '<output_dir>/tpcectl.pid'
```

Native PID files written by the binaries (`bh.pid`, `mee.pid`, and `driver.pid`) are also used when available.

The `database.password_env` secret is not placed in run-config, argv, or a shell command. If the runtime service account's environment does not already provide the value, the orchestrator:

1. reads the named environment variable only on the control host;
2. creates a per-instance file `<remote_root>/.tpcectl/secrets/<run_id>-<instance>.env` with mode 0600 through SFTP;
3. uses a wrapper that sources the file without echo or xtrace, deletes it before `exec` of the runtime, and supplies the value only through the process environment;
4. deletes the temporary secret file after any startup failure.

Secret files are not part of the deployment manifest and are never stored in run-state, collected results, or logs. A missing value required by a BH is a startup error detected before the process starts.

### 9.2. Generating run-config

Before distributing `run-config.json`:

```text
endpoint_sets.bh  = [ host_address(bh.host) + ":" + bh.listen   for bh in profile.bh  ]
endpoint_sets.mee = [ host_address(mee.host) + ":" + mee.listen for mee in profile.mee ]
```

These values are written as the JSON arrays `endpoint_sets.bh` and `endpoint_sets.mee`, not inserted into argv. Use the **address** from `hosts`, not the logical name when name ≠ address.

### 9.3. Normative argv

Every orchestrated process receives the same `--run-config` path plus **role-specific per-instance flags**. Global test parameters come only from the file; variable instance parameters come only from argv.

`remote_run_config = <remote_root>/runs/<run_id>/run-config.json`.

**BrokerageHouse** (`bh`):

```text
BrokerageHouseMain-pgsql.exe \
  --run-config <remote_run_config> \
  --instance <bh.name> \
  -l <bh.listen> \
  -o <expanded bh.output> \
  --ready-file <expanded bh.output>/.service-ready \
  --pool-init-timeout <2 * timeouts.ready, seconds>
```

Outgoing MEE pool: `endpoint_sets.mee` from the file. `client_side` is read from the file (`true` in v1).

**MarketExchange** (`mee`):

```text
MarketExchange.exe \
  --run-config <remote_run_config> \
  --instance <mee.name> \
  -l <mee.listen> \
  -U <mee.unique_id> \
  -o <expanded mee.output> \
  --ready-file <expanded mee.output>/.service-ready \
  --pool-init-timeout <timeouts.ready, seconds>
```

Outgoing BH pool: `endpoint_sets.bh` from the file. `base_time_epoch` is read from the file.

**Driver** (`dm`):

```text
Driver.exe \
  --run-config <remote_run_config> \
  --role dm \
  --instance <dm.name> \
  -o <expanded dm.output> \
  --pool-init-timeout <timeouts.ready, seconds> \
  -d <calculated dm.duration_sec>
```

Outgoing BH pool: `endpoint_sets.bh` from the file.

**Driver** (`ce`):

```text
Driver.exe \
  --run-config <remote_run_config> \
  --role ce \
  --instance <ce.name> \
  -u <ce.users> \
  --ce-id-base <ce.ce_id_base> \
  -o <expanded ce.output> \
  --pool-init-timeout <timeouts.ready, seconds>
```

When `ce.partition` is present in the profile, append:

```text
  --ce-start-id <start_id> --ce-part-count <count> --ce-part-percent <percent>
```

Measurement duration `-d` is **not** passed for CE; the runtime reads `scale.duration_sec` from the file.

**Driver** (`standalone`):

```text
Driver.exe \
  --run-config <remote_run_config> \
  --role standalone \
  --instance standalone \
  -u <standalone_driver.users> \
  --ce-id-base <standalone_driver.ce_id_base> \
  -o <expanded standalone_driver.output> \
  --pool-init-timeout <timeouts.ready, seconds> \
  -d <standalone_driver.duration_sec>
```

For standalone, `--instance` MUST be exactly `standalone`.

`dm.duration_sec = cleanup_wait + scale.duration_sec + ce_completion_grace + mee_drain + stop_grace`; all values are rounded up to seconds and passed as `-d`. This is a conservative upper bound; in normal operation, the orchestrator stops the DM earlier, after CE completion and drain.

Mixing `--run-config` with duplicate **global** flags (`-c`, `-t`, `-f`, `-w`, `-i`, `-h`, `-p`, `--bh-endpoints`, `--base-time-epoch`, and similar) is a usage error. Per-instance flags in this section are required where shown.

**Loader** shard, unchanged:

```text
Loader.exe -l CUSTOM -i <remote_root>/data \
  -b <begin> -c <count> -t <total_customers> \
  -f <scale_factor> -w <initial_trade_days> \
  -p "<conninfo>"
```

### 9.4. BaseTime

1. If `base_time_epoch` is set in the profile or through `--base-time-epoch`, use it.
2. Otherwise, before generating run-config, calculate `epoch = now_utc + timeouts.config_distribute + 2 * timeouts.ready + base_time_lead_sec`. These intervals reserve time for configuration distribution, BH listen-ready, MEE/pool bootstrap, and clock/network margin.
3. Write the same epoch to immutable `run-config.json` and local `run-state.json`; every MEE reads it from the file.
4. Before run-config distribution, an explicitly specified epoch must be no earlier than `now_utc + timeouts.config_distribute + 2 * timeouts.ready + 5s`; otherwise, the run exits before any remote process starts.
5. An MEE becomes service-ready only after pool-ready, arrival of the epoch, and its call to `SetBaseTime()`. If the epoch passes before bootstrap completes, the MEE exits with an error; the orchestrator does not calculate a new epoch within the same run.
6. `plan` prints the selected epoch. For a reproducible `plan` without a run, it uses an epoch specified in the profile or CLI. When the epoch is calculated dynamically, `plan` prints the formula and labels the value as a preview.
7. MEE host clocks must be synchronized. Before a multi-MEE run, the orchestrator compares UTC time through SSH; an absolute difference greater than one second is a validate-at-run error.

### 9.5. Waiting for Trade-Cleanup

After starting the DM, the orchestrator does **not** start CE instances until Trade-Cleanup completes.

The normative detection method is the complete line `Trade-Cleanup transaction completed.` in DM stdout. The search covers only bytes appended by the current process after its startup; it does not use an old log. DM termination without the marker is an error. Expiry of `timeouts.cleanup_wait` is an error and causes DM/MEE/BH to stop. The runtime may add a separate ready file in the future, but it does not replace the line marker until the contract version changes.

### 9.6. `stop`

The only valid sequence is CE → wait for `timeouts.mee_drain` while DM/MEE/BH remain running → DM → MEE → BH.
For every process: send SIGTERM, wait for `timeouts.stop_grace`, send SIGKILL, and then verify that the process has terminated. Do not use `killall` by default. Repeated `stop` is idempotent: an already terminated PID is not an error. Before sending a signal, the orchestrator must use local `run-state.json` to verify that the PID belongs to the expected run and process.

In a standalone profile, the process with role `standalone` receives SIGTERM during the CE step. The separate DM step is skipped because the CE and DM are in the process that has already stopped. The MEE drain interval and the subsequent stop steps for MEE and BH are unchanged.

---

## 10. Validate: Extended Rules

`tpcectl validate` must verify:

1. `customers`, `active_customers`, and load shard counts are divisible by 1000; the sum of shard `count` values equals `customers`.
2. There is exactly one DM source: either one `dm` object with separate CE entries, or one `standalone_driver` with no `dm` or `ce` entries. Multiple `standalone` instances are not allowed.
3. Every `mee[].unique_id` is unique and ≥ 1.
4. All timeouts are positive; `base_time_lead_sec >= 5`; an explicitly specified `base_time_epoch` is also checked immediately before a run under §9.4.
5. Partitioning is optional, including with multiple CE instances. Exactly two modes are allowed: every CE omits partition, or every CE specifies partition. In the second mode, `percent == 50`, `start_id % 1000 == 1`, `count >= 5000`, and `count % 1000 == 0`; ranges do not overlap, and their union in a compliant profile is exactly `[1 .. customers]`, with no gaps or duplicates. Mixed mode is rejected.
6. `ce_id_base >= 1`; a CE with `users=N` occupies the half-open interval `[ce_id_base, ce_id_base+N)`. Intervals for different CE instances do not overlap.
   `standalone_driver` must have `ce_id_base >= 1`; the recommended example value is `1`. A profile containing both `standalone_driver` and separate `ce` entries is invalid under rule 2.
7. `|bh| >= 1` and `|mee| >= 1`; `endpoint_sets.bh/mee` contain the complete address sets, and every instance reference points to the correct nonempty set.
8. Reject profiles in which listening ports on the same host conflict.
9. `client_side: true` in v1.
10. Validation messages cover the prohibited configurations in [`spec-scalability.md`](spec-scalability.md) §7, including multiple DM instances, partition percent ≠ 50, duplicate MEE UniqueId values, and similar errors.

---

## 11. Schema and Load

### 11.1. `schema`

Apply `create_tables.sql` or `create_tables_partitioned.sql`, plus indexes and foreign keys as configured in the profile.
`schema --recreate` is available only with an explicit flag.

### 11.2. `load`

Run shards in parallel, in the foreground, and fail fast. After loading, apply indexes and foreign keys if they were deferred.

---

## 12. Collect

1. Using local `run-state.json`, download every `output` from `bh`, `mee`, `dm`, and `ce` instances, including standalone, for the current `run_id`.
2. Save a redacted profile, immutable `run-config.json`, and the final local `run-state.json`.
3. Post-processing tpsE or transaction mix is **not** required for the orchestrator MVP; do not break the log format. `collect.post_command` is optional.
4. Future work: exporting Trade-Results per Load Unit per minute for EGenTester, as described in spec-scalability §5.7, is a separate task. The orchestrator preserves the complete raw CE logs.

---

## 13. Cleanup

Delete only paths listed in `deploy-manifest.json`, plus `runs/` when explicitly requested by a flag. Do not drop the database. Require `--yes` in a non-TTY environment.

---

## 14. Deployment Manifest and Local Run-State

### 14.1. Deployment Manifest

Each runtime host atomically stores `<remote_root>/.tpcectl/deploy-manifest.json` in the following form:

```json
{
  "schema_version": 1,
  "profile_name": "lab-50k-scale",
  "host": "mid1",
  "remote_root": "/opt/tpce",
  "deployed_at": "2026-07-17T10:00:00Z",
  "complete": true,
  "entries": [
    {"path": "bin/BrokerageHouseMain-pgsql.exe", "type": "file", "sha256": "...", "created_by_tpcectl": true},
    {"path": "bin", "type": "dir", "created_by_tpcectl": true}
  ]
}
```

Rules:

1. `path` is always relative and normalized, without `..`; absolute paths are prohibited.
2. `cleanup` deletes only the listed file and symlink entries under an exactly matching `remote_root`. Directories are deleted after files, and only when `created_by_tpcectl=true` and the directory is empty.
3. Normal cleanup does not delete `remote_root` itself, `.tpcectl`, or `runs/`. Deleting `runs/<run_id>` requires a separate explicit flag with the specified run ID.
4. Before the first remote mutation, create a deployment manifest with `complete:false`; after each entry is created successfully, update it atomically. After deployment succeeds on that host, write `complete:true`. This allows cleanup to safely remove a partial deployment.
5. A repeated deployment updates checksums and metadata for existing entries and retains entries still managed by the profile.
6. A mismatched `host` or `remote_root`, or an unsupported `schema_version`, blocks cleanup.

`deploy-manifest.json` is not shared run status. It is an independent, host-local file-ownership journal used for safe cleanup. Its copies MUST NOT be synchronized between hosts.

### 14.2. Local Orchestrator Run-State

`last-run.json` and `run-manifest.json` are not used. The only canonical mutable run-state exists on the control host:

```text
<state-dir>/runs/<run_id>/run-state.json
```

```json
{
  "schema_version": 1,
  "run_id": "20260717T100000Z",
  "profile_path": "/home/user/tpce/profiles/lab.yaml",
  "profile_sha256": "...",
  "run_config_sha256": "...",
  "remote_run_config": "/opt/tpce/runs/20260717T100000Z/run-config.json",
  "base_time_epoch": 1784282520,
  "started_at": "2026-07-17T10:01:30Z",
  "state": "running",
  "processes": [
    {
      "role": "bh",
      "name": "bh1",
      "host": "mid1",
      "pid": 12345,
      "pid_file": "/opt/tpce/runs/20260717T100000Z/bh1/tpcectl.pid",
      "output": "/opt/tpce/runs/20260717T100000Z/bh1",
      "started_at": "2026-07-17T10:01:31Z"
    }
  ]
}
```

State rules:

1. Create `run-state.json` before the first remote process mutation. Update it atomically after every start and stop, and during transitions `starting → running → stopping → completed|failed`.
2. The file and its parent directory have modes 0600 and 0700, respectively. State is not copied to runtime hosts, in whole or in part.
3. `status`, `stop`, and `collect` read state only from `--state-dir`. Without `--run-id`, the run is resolved through local `profiles/<profile-id>/current-run.json`; with `--run-id`, it is read from local path `runs/<run_id>/run-state.json`.
4. A profile may have at most one state in `starting|running|stopping`; a local file lock enforces this. A new run requires the preceding run to finish or to be explicitly marked failed.
5. Runtime PID, readiness, and log files on hosts are process-local artifacts, not shared status. The orchestrator checks them and reflects their state in local run-state.
6. If local state is lost, automatic `stop` and `cleanup runs` are prohibited: the orchestrator does not scan PIDs or invoke `killall`. Recovery requires a separate explicit procedure using a known `run_id` and remote PID files.
7. In a standalone profile, the process entry has role `standalone`; there are no separate `dm` or `ce` entries.

---

## 15. Error Handling and Observability

- Logs include host, argv with passwords redacted, exit code, and the tail of stderr.
- Parallelism is allowed only **within one step or role**: all BH instances in parallel, then all MEE instances in parallel, then one DM, then all CE instances in parallel. Parallel startup of different roles is prohibited.
- Ctrl+C during `run` performs stop in the §9.6 sequence.
- The readiness check has two levels: a successful TCP connection means listen-ready; `<output>/.service-ready` means full readiness. The DM MUST NOT start before every BH and MEE instance is service-ready.

---

## 16. Go Module Structure

```text
tools/tpcectl/
  go.mod
  cmd/tpcectl/main.go
  internal/
    config/       # YAML + validation, including partition and DM uniqueness
    runtimeconfig/# deterministic run-config JSON + SHA-256
    state/        # only local control host run-state + locks/index
    sshx/
    deploy/
    process/      # nohup start/stop/status
    endpoints/    # endpoint_sets for run-config
    argv/         # --run-config + per-instance CLI flags (§9.3)
    schema/
    load/
    run/          # sequence BH → MEE → DM Trade-Cleanup wait → CE
    collect/
    cleanup/
    plan/
  profiles/
    smoke-single.yaml
    lab-multi.yaml
  README.md
```

---

## 17. MVP Acceptance Criteria

1. `validate` rejects two DM instances, overlapping CE partitions, and duplicate MEE `unique_id` values.
2. `plan run` prints the complete sequence, the global `run-config.json` (redacted), its target path and hash, and the full per-instance argv from §9.3.
3. `deploy` followed by `cleanup --yes` on at least two hosts deletes every managed entry but preserves `remote_root`, `.tpcectl`, and `runs/`.
4. A partially failed deployment leaves `complete:false`; subsequent cleanup deletes the recorded managed entries.
5. Multi-instance smoke test after the binaries are ready: 2 BH + 2 MEE + 1 DM + 2 CE → short duration → collect, with no startup errors.
6. The DM does not start before every BH/MEE is service-ready; CE instances do not start before the DM's Trade-Cleanup marker.
7. Every MEE receives the same `base_time_epoch` and becomes service-ready only after `SetBaseTime()`.
8. `run` waits for every CE to exit, performs drain, and stops roles in the sequence CE→DM→MEE→BH.
9. Every runtime host receives a byte-identical global `run-config.json`; per-instance settings appear only in argv, not in the file.
10. `run-state.json` exists only under local `--state-dir`; runtime hosts contain no `last-run.json`, `run-manifest.json`, or copies of shared state.
11. The database password is absent from run-config, run-state, and logs.
12. `go build` succeeds on macOS and Linux without Ansible.

---

## 18. Implementation Milestones

| Milestone | Scope |
| --- | --- |
| M1 | profile configuration + validation + run-config generation + local run-state/lock + plan + SSH |
| M2 | deploy + cleanup + deployment manifest |
| M3 | run-config distribution + per-instance argv (§9.3) + start/stop/status for BH, MEE, DM, and CE |
| M4 | schema + load |
| M5 | run: BaseTime, Trade-Cleanup barrier, collect, and smoke profiles |
| M6 | polish: agent authentication, tar upload, redaction, and EGenTester export hook |

Dependency: a complete M5 multi-instance smoke test requires the P0/P1 changes in [`spec-scalability.md`](spec-scalability.md) §8. Until they are ready, use `standalone_driver` + 1 BH + 1 MEE.

---

## 19. Open Items

1. TLS for PostgreSQL: only `deploy.artifacts` + DSN parameters.
2. Implement schema operations through `jackc/pgx` or invoke `psql`.

---

## 20. Related Documents

- Runtime scalability: [`spec-scalability.md`](spec-scalability.md), the **source of truth** for the CLI and process topology.
- DDL: [`sql/pgsql/README.md`](../sql/pgsql/README.md).
- Building the binaries: `./ya make dbt5`.
- Root README: repository overview.
