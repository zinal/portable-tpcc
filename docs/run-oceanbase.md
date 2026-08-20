# Running TPC-C against OceanBase

Binary: `tpcc-oceanbase`. Full parameter list:
[parameter-reference.md](parameter-reference.md).

Two modes:

1. **Standalone** — drive `tpcc-oceanbase` directly (simplest for local smoke tests).
2. **Orchestrated** — use `mind-tpcc` with a YAML profile (multi-host and full
   pipeline).

Results MUST NOT be called official TPC-C results without the required TPC
verification.

## Build

From the repository root:

```bash
./ya make tpcc/app/oceanbase
go -C mind build ./cmd/mind-tpcc
```

Or build everything with `./build.sh` (it always passes
`-DHAVE_CUDA=no -DCUDA_VERSION=11.4`, required when the YDB target is in the
graph). The adapter talks through the vendored OceanBase Connector/C
(`contrib/restricted/obconnector-c`).

Binaries:

| Binary | Path after build |
| --- | --- |
| `tpcc-oceanbase` | `tpcc/app/oceanbase/tpcc-oceanbase` |
| `mind-tpcc` | `mind/mind-tpcc` |

For orchestration, copy `tpcc-oceanbase` into the profile's
`paths.local_artifacts` directory (for example `./dist/tpcc-oceanbase`).
Re-run `mind-tpcc deploy` after rebuilding; `run` does not auto-upload
binaries.

## Prepare OceanBase

Use a reachable OceanBase tenant over the MySQL-compatible SQL port
(default **2881**). Typical lab user form is `user@tenant`.

The target TPC-C database is created automatically
(`CREATE DATABASE IF NOT EXISTS`) when schema runs, so you do not need to
pre-create it. `database.database` is the connection `database=` value;
`database.path` is the database that holds TPC-C tables (they are often the
same name).

Do not put passwords in profile YAML/JSON. Pass them via the connection string
(standalone) or an environment variable named in `password_env` on the control
host (orchestrated). `mind-tpcc` delivers the value as a worker-local
`password_file` (`db-password`, mode 0600), not via the remote process command
line.

Client user: profile `database.user`, else `TPCC_OB_USER`, else `root@root`.

## OceanBase-specific settings

| Standalone | Profile | Meaning |
| --- | --- | --- |
| `--connection` | `database.endpoint` + `database` + `user` + `password_env` | Connector string vs orchestrated `host` or `host:port` (default port **2881**). Endpoint must not contain credentials. |
| `-p` / `--path` | `database.path` | Database name for TPC-C tables (`CREATE DATABASE IF NOT EXISTS`). |
| `--partitions` | `database.options.partitions` | HASH partition count. See below. |
| `--foreign-keys` | `database.options.foreign_keys` | `on` (default) or `off`. |
| `--index-parallel` | `database.options.index_parallel` | `CREATE INDEX … PARALLEL n` (default **4**, `1` = serial). |
| `query_timeout=` in `--connection` | `database.options.query_timeout` | Session `ob_query_timeout` for bulk import / `CREATE INDEX` / `DBMS_STATS` / integrity checks, in **seconds** (default **600**). OceanBase server default is 10s. |

On a non-OceanBase MySQL server the partition options are ignored.

Unknown `database.options.*` keys are rejected for `dbms=oceanbase`.
`database.auth_scheme`, `sa_key_file`, and `ca_file` are YDB-only.

### Schema partitioning

OceanBase creates a binding `TABLEGROUP` and HASH-partitions warehouse-scoped
tables by warehouse id (`w_id` / `*_w_id`). Partition count is set **only at
schema time** (`tpcc-oceanbase schema` / `mind-tpcc` schema stage). The same
value is reused at `indexes` as the `DBMS_STATS.GATHER_TABLE_STATS` degree of
parallelism (`1` when partitioning is off).

Integrity checks scan warehouse-scoped tables one `w_id` at a time (same
chunk size as PostgreSQL and YDB) and open `--threads` parallel sessions so
HASH partition pruning can apply. Catalog ids run one after another; the
parallel sessions apply to the current id's warehouse chunks, so
`Checking … [OK]` appears as each check finishes. Under `mind-tpcc`,
`--threads` comes from CLI `--threads` when set, otherwise
`runtime.check_concurrency` (`0` / omit = `min(scale.warehouses, 32)`).
TPC-C §3.3.2 predicates are unchanged.

| Value | Meaning |
| --- | --- |
| `-1` | No tablegroup / no HASH partitions (plain tables) |
| `0` (default) | Derive partition count from warehouse scale (`max(1, warehouses)`) |
| `N` (`1`…`8192`) | Explicit HASH partition count |

```bash
# default: partitions == warehouses (here 10)
./tpcc/app/oceanbase/tpcc-oceanbase schema --connection="$CONN" --path=tpcc \
  -w 10 --partitions=0

# explicit count
./tpcc/app/oceanbase/tpcc-oceanbase schema --connection="$CONN" --path=tpcc \
  -w 100 --partitions=64

# plain (non-partitioned) tables
./tpcc/app/oceanbase/tpcc-oceanbase schema --connection="$CONN" --path=tpcc \
  -w 10 --partitions=-1
```

```yaml
database:
  options:
    partitions: 64   # or 0 to derive from scale.warehouses, or -1 to disable
```

## Standalone local run

```bash
CONN='host=127.0.0.1;port=2881;user=root@test;password=YOUR_PASSWORD;database=tpcc'
BIN=./tpcc/app/oceanbase/tpcc-oceanbase

# schema (hash partitions derived from -w; --foreign-keys=off omits FKs)
$BIN schema --connection="$CONN" --path=tpcc -w 10 \
  --partitions=0 --foreign-keys=off
# optional explicit count (otherwise derived from -w when --partitions=0):
#   --partitions=64
# plain tables:
#   --partitions=-1

# load
$BIN import --connection="$CONN" --path=tpcc -w 10 -t 8

# indexes + statistics (after load); CREATE INDEX uses PARALLEL 4 by default.
# DBMS_STATS gather DOP equals HASH partition count (--partitions / -w; 1 if -1).
$BIN indexes --connection="$CONN" --path=tpcc -w 10 --partitions=0
#   --index-parallel=8   # raise DOP for a single CREATE INDEX
#   --index-parallel=1   # serial index build

# check after load ( -t N parallel sessions; omit for a single session )
$BIN check --connection="$CONN" --path=tpcc -w 10 -t 10 --after-import

# measurement run (durations in minutes)
$BIN run --connection="$CONN" --path=tpcc -w 10 \
  --duration=5 -t 4

# check after run
$BIN check --connection="$CONN" --path=tpcc -w 10 -t 10 --after-run

# drop TPC-C tables
$BIN clean --connection="$CONN" --path=tpcc
```

Connection string also accepts `query_timeout=<seconds>` (default 600).

Useful shared flags: `--no-delays`, `--help`.
See [parameter-reference.md](parameter-reference.md#standalone-tpcc-dbms-cli).

## Orchestrated run (`mind-tpcc`)

Minimum profile fields: `apiVersion` / `kind` / `metadata.name`, `ssh`,
`paths`, `database`, `scale.warehouses`, non-empty `loaders` and `workers`,
and `phases` durations. Omitted workload fields use built-in TPC-C 5.11-style
defaults. Full schema: [parameter-reference.md](parameter-reference.md#profile-yaml).

A ready example lives in
[`docs/examples/profile.oceanbase.v1.yaml`](examples/profile.oceanbase.v1.yaml):

```yaml
database:
  dbms: oceanbase
  endpoint: 127.0.0.1:2881      # host or host:port; default port 2881
  database: tpcc                # connection database=
  path: tpcc                    # TPC-C tables database (CREATE IF NOT EXISTS)
  user: root@root               # optional; else TPCC_OB_USER, else root@root
  password_env: TPCC_PASSWORD
  options:
    partitions: 0               # -1 off, 0 derive from scale.warehouses, N explicit (max 8192)
    foreign_keys: off           # omit FKs at schema time; default on
    query_timeout: 600          # bulk import / CREATE INDEX / DBMS_STATS / check (seconds)
    index_parallel: 4           # CREATE INDEX DOP; default 4, 1 = serial

scale:
  warehouses: 10

checks:
  after_import: true
  after_run: true
```

```bash
export TPCC_PASSWORD='...'
# export TPCC_OB_USER='root@root'   # optional when database.user is omitted

mkdir -p dist
cp tpcc/app/oceanbase/tpcc-oceanbase dist/
cp mind/mind-tpcc ./mind-tpcc

# cp docs/examples/profile.oceanbase.v1.yaml ./profile-oceanbase.yaml

./mind-tpcc validate --profile ./profile-oceanbase.yaml
./mind-tpcc plan     --profile ./profile-oceanbase.yaml
./mind-tpcc deploy --profile ./profile-oceanbase.yaml
./mind-tpcc run --profile ./profile-oceanbase.yaml
```

Or run stages individually: `deploy`, `schema`, `load`, `indexes`,
`check --after-import`, `start`, `check --after-run`, `collect`,
`consolidate`.

`mind-tpcc run` includes `check(after-import)` / `check(after-run)` only when
`checks.after_import` / `checks.after_run` are true.

Artifacts land under `paths.result_root/<run_id>/` (including
`aggregate.json`, `orchestrator/run-config.json`, and
`profile.redacted.yaml`).

On a single host, set every loader/worker `host` to `127.0.0.1` (local
sessions, no SSH). Multi-host runs need SSH access and tightly synchronized
clocks.

### Multiple workers / co-location

`host` is the connection address. Mind splits `scale.warehouses` into balanced
contiguous ranges across instances. Reuse the same `host` string to co-locate
several loaders/workers on one machine (one SSH/local session).

```yaml
scale:
  warehouses: 300

loaders:
  - name: loader-a
    host: 10.10.0.21
  - name: loader-b
    host: 10.10.0.22

workers:
  - name: worker-a
    host: 10.10.0.31
  - name: worker-b
    host: 10.10.0.31   # co-located with worker-a
  - name: worker-c
    host: 10.10.0.32

runtime:
  threads_per_loader: 4   # 0 / omit = auto (CPU-capped per loader process)
  threads_per_worker: 4
  check_concurrency: 0    # 0 / omit = auto (min of warehouses and 32)
  max_inflight_per_worker: 256
```

With three workers and 300 warehouses, each worker typically owns a contiguous
block of 100 warehouses. For a single-host smoke test, set every `host` to
`127.0.0.1` and still declare multiple `workers` / `loaders` entries — mind
launches one process per instance.

Orchestrated roles launched by mind (for reference):

```text
tpcc-oceanbase schema --run-config run-config.json --instance schema-0
tpcc-oceanbase loader --run-config run-config.json --instance <loader>
tpcc-oceanbase indexes --run-config run-config.json --instance indexes-0
tpcc-oceanbase worker --run-config run-config.json --instance <worker> --start-at=<UTC>
tpcc-oceanbase check  --run-config run-config.json --instance check-0 --after-import|--after-run [--threads=N]
tpcc-oceanbase clean  --run-config run-config.json --instance clean-0   # mind-tpcc cleanup
```

```bash
./mind-tpcc cleanup --profile ./profile-oceanbase.yaml --yes
./mind-tpcc undeploy --profile ./profile-oceanbase.yaml --yes
```

`cleanup` drops TPC-C objects (when the run is past deploy), remote
`remote_root/<run_id>`, and local results + state. Shared worker binaries stay
installed until `undeploy`.

## Checklist

1. OceanBase is reachable on the MySQL SQL port (default 2881).
2. `tpcc-oceanbase` is built (and `mind-tpcc` for orchestration).
3. Credentials are supplied (`--connection=...` or `TPCC_PASSWORD`;
   optional `database.user` / `TPCC_OB_USER`).
4. Binary is under `paths.local_artifacts` as `tpcc-oceanbase`.
5. Flow: `schema` → `import`/`load` → `indexes` → `check --after-import`
   → `run`/`start` → `check --after-run`.

For a quick engineering smoke test, standalone with `-w 10` and a short
`--duration` is enough. For settings closer to TPC-C 5.11, see the defaults
embedded in `mind-tpcc` and
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).
