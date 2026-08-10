# portable-tpcc

A horizontally scalable TPC-C implementation with shared workload logic,
YDB/PostgreSQL/OceanBase adapters, and a dedicated orchestrator.

Architecture draft:

- [specification](docs/specification.md);
- [shared libraries and adapter API](docs/adapter-api.md);
- [profile example (YDB)](docs/examples/profile.v1.yaml);
- [profile example (OceanBase)](docs/examples/profile.oceanbase.v1.yaml);
- [run-config example](docs/examples/run-config.v1.json);
- [start-token example](docs/examples/start-token.v1.json);
- [aggregate example](docs/examples/aggregate.v1.json).

Implementation status and third-party dependencies:
[docs/dependencies.md](docs/dependencies.md). Shared libraries and adapter API:
[docs/adapter-api.md](docs/adapter-api.md). Alignment plan (accepted API
decisions and phase checklist):
[docs/alignment-plan.md](docs/alignment-plan.md). Engineering vs TPC-C 5.11
conformance notes:
[docs/tpcc-5.11-conformance-analysis.md](docs/tpcc-5.11-conformance-analysis.md).

Results MUST NOT be called official TPC-C results without the required TPC
verification.

## Running TPC-C against PostgreSQL

Two modes are available:

1. **Standalone** — drive `tpcc-pgsql` directly (simplest for local smoke tests).
2. **Orchestrated** — use `mind-tpcc` with a YAML profile (multi-host and full
   pipeline).

### Build

From the repository root:

```bash
./ya make tpcc/app/pgsql
go -C mind build ./cmd/mind-tpcc
```

The YDB binary requires CUDA to be disabled in ya make:

```bash
./ya make -r -DHAVE_CUDA=no -DCUDA_VERSION=11.4 tpcc/app/ydb
```

Or build everything with `./build.sh` (it always passes the CUDA defines
above). The PostgreSQL worker/loader binary is `tpcc-pgsql`. For
orchestration, place it under the profile's `paths.local_artifacts`
directory (for example `./dist/tpcc-pgsql`).

### Prepare PostgreSQL

```bash
createdb tpcc
```

Do not put passwords in profile YAML/JSON. Pass them via the connection
string (standalone) or an environment variable named in `password_env`
(orchestrated).

### Standalone local run

```bash
CONN='host=localhost port=5432 dbname=tpcc user=postgres password=YOUR_PASSWORD'

# schema (unpartitioned; --foreign_keys=off omits FKs, default is on)
./tpcc-pgsql schema --connection="$CONN" --path=portable_tpcc -w 10 \
  --foreign_keys=off

# schema with HASH partitions by warehouse id (also without FKs)
./tpcc-pgsql schema --connection="$CONN" --path=portable_tpcc -w 10 \
  --partitioning=warehouse_hash --foreign_keys=off
# optional explicit modulus (otherwise derived from -w):
#   --partition-count=64

# load
./tpcc-pgsql import --connection="$CONN" --path=portable_tpcc -w 10 -t 8

# check after load
./tpcc-pgsql check --connection="$CONN" --path=portable_tpcc -w 10 --after-import

# measurement run (durations in minutes)
./tpcc-pgsql run --connection="$CONN" --path=portable_tpcc -w 10 \
  --duration=5 -t 4

# check after run
./tpcc-pgsql check --connection="$CONN" --path=portable_tpcc -w 10 --after-run

# drop TPC-C tables
./tpcc-pgsql clean --connection="$CONN" --path=portable_tpcc
```

Useful flags:

- `--partitioning=warehouse_hash` — create warehouse-scoped tables as HASH
  partitions (`stock`, `customer`, `history`, `oorder`, `new_order`,
  `order_line`); `warehouse` / `district` / `item` stay unpartitioned.
  Optional `--partition-count=N` sets the modulus; if omitted, `N` is
  derived from `-w` / `--warehouses`. See
  [docs/pgsql-partitioning-design.md](docs/pgsql-partitioning-design.md).
- `--foreign_keys=off` — omit FOREIGN KEY constraints at schema time
  (default `on`). Idempotent load still replaces warehouse ranges via
  explicit deletes.
- `--no-delays` — disable keying/think time (engineering runs);
- `--help` — full command list.

### Orchestrated run (`mind-tpcc`)

Use a profile with `database.dbms: pgsql`. A minimal example lives in
[`mind/testdata/profile.valid.yaml`](mind/testdata/profile.valid.yaml):

```yaml
database:
  dbms: pgsql
  endpoint: localhost:5432          # host or host:port; no user=/password=
  database: tpcc
  path: portable_tpcc
  password_env: TPCC_PASSWORD
  options:
    partitioning: warehouse_hash    # omit or "none" for unpartitioned tables
    # partition_count: 64           # optional; else derived from scale.warehouses
    foreign_keys: off               # omit FKs at schema time; default on
```

For PostgreSQL, `database.user` is not set in the profile. The client user
defaults to `postgres`, or to the value of `TPCC_PG_USER` when that env var
is set.

```bash
export TPCC_PASSWORD='...'
# export TPCC_PG_USER=myuser   # if not postgres

mkdir -p dist && cp /path/to/tpcc-pgsql dist/
# After `go -C mind build ./cmd/mind-tpcc`, the binary is at mind/mind-tpcc:
cp mind/mind-tpcc ./mind-tpcc

./mind-tpcc validate --profile ./profile-pgsql.yaml
./mind-tpcc plan     --profile ./profile-pgsql.yaml

# Full pipeline:
# validate → deploy → schema → load → check(after-import)
# → start → check(after-run) → collect → consolidate
./mind-tpcc run --profile ./profile-pgsql.yaml
```

Or run stages individually: `deploy`, `schema`, `load`,
`check --after-import`, `start`, `check --after-run`, `collect`,
`consolidate`.

Artifacts land under `paths.result_root/<run_id>/` (including
`aggregate.json`).

On a single host, set every loader/worker `host` to `127.0.0.1` (local
sessions, no SSH). Multi-host runs need SSH access and tightly synchronized
clocks. Identical `host` values mean co-location on one machine.

### Checklist

1. PostgreSQL is reachable and the database exists.
2. `tpcc-pgsql` is built (and `mind-tpcc` for orchestration).
3. Credentials are supplied (`--connection=...` or `TPCC_PASSWORD`).
4. Flow: `schema` → `import`/`load` → `check --after-import` → `run`/`start`
   → `check --after-run`.

For a quick engineering smoke test, standalone with `-w 10` and a short
`--duration` is enough. For settings closer to TPC-C 5.11, see the defaults
embedded in `mind-tpcc` and
[docs/tpcc-5.11-conformance-analysis.md](docs/tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).

## Running TPC-C against OceanBase

Two modes are available:

1. **Standalone** — drive `tpcc-oceanbase` directly (simplest for local smoke tests).
2. **Orchestrated** — use `mind-tpcc` with a YAML profile (multi-host and full
   pipeline).

### Build

From the repository root:

```bash
./ya make tpcc/app/oceanbase
go -C mind build ./cmd/mind-tpcc
```

Or build everything with `./build.sh` (it always passes the CUDA defines
needed when the YDB target is in the graph). The OceanBase worker/loader
binary is `tpcc-oceanbase`. For orchestration, place it under the profile's
`paths.local_artifacts` directory (for example `./dist/tpcc-oceanbase`).

### Prepare OceanBase

Use a reachable OceanBase tenant over the MySQL-compatible SQL port
(default **2881**). The adapter talks through the vendored OceanBase
Connector/C (`contrib/restricted/obconnector-c`).

Typical lab user form is `user@tenant`. In orchestrated mode the client user
comes from `database.user`, else `TPCC_OB_USER`, else `root@root`.
The target database is created automatically (`CREATE DATABASE IF NOT EXISTS`)
when schema runs, so you do not need to pre-create it.

Do not put passwords in profile YAML/JSON. Pass them via the connection
string (standalone) or an environment variable named in `password_env`
(orchestrated).

### Standalone local run

```bash
CONN='host=127.0.0.1;port=2881;user=root@test;password=YOUR_PASSWORD;database=tpcc'

# schema (hash partitions derived from -w; --foreign-keys=off omits FKs)
./tpcc-oceanbase schema --connection="$CONN" --path=tpcc -w 10 \
  --partitions=0 --foreign-keys=off

# load
./tpcc-oceanbase import --connection="$CONN" --path=tpcc -w 10 -t 8

# check after load
./tpcc-oceanbase check --connection="$CONN" --path=tpcc -w 10 --after-import

# measurement run (durations in minutes)
./tpcc-oceanbase run --connection="$CONN" --path=tpcc -w 10 \
  --duration=5 -t 4

# check after run
./tpcc-oceanbase check --connection="$CONN" --path=tpcc -w 10 --after-run

# drop TPC-C tables
./tpcc-oceanbase clean --connection="$CONN" --path=tpcc
```

Useful flags:

- `--partitions` — OceanBase hash partitions via tablegroup:
  `-1` = plain tables, `0` = derive from `-w` / `--warehouses`,
  `N` = explicit partition count (max 8192). On a non-OceanBase MySQL
  server the partition options are ignored.
- `--foreign-keys=off` — omit FOREIGN KEY constraints at schema time
  (default `on`).
- `--no-delays` — disable keying/think time (engineering runs);
- `--help` — full command list.

### Orchestrated run (`mind-tpcc`)

Use a profile with `database.dbms: oceanbase`. A ready example lives in
[`docs/examples/profile.oceanbase.v1.yaml`](docs/examples/profile.oceanbase.v1.yaml):

```yaml
database:
  dbms: oceanbase
  endpoint: 127.0.0.1:2881      # host or host:port; default port 2881
  database: tpcc                # connection database=
  path: tpcc                    # TPC-C tables database (CREATE IF NOT EXISTS)
  user: root@root               # optional; else TPCC_OB_USER, else root@root
  password_env: TPCC_PASSWORD
  options:
    partitions: 0               # -1 off, 0 derive from warehouses, N explicit
    foreign_keys: off           # omit FKs at schema time; default on
```

For OceanBase, optional `database.user` sets the client login (`user@tenant`).
When omitted, `tpcc-oceanbase` uses `TPCC_OB_USER` if set, otherwise
`root@root`. Password is read from the env named in `password_env` and
injected into remote role processes by `mind-tpcc`.

```bash
export TPCC_PASSWORD='...'
# export TPCC_OB_USER='root@root'   # optional when database.user is omitted

mkdir -p dist
cp tpcc/app/oceanbase/tpcc-oceanbase dist/
# After `go -C mind build ./cmd/mind-tpcc`, the binary is at mind/mind-tpcc:
cp mind/mind-tpcc ./mind-tpcc

# Copy or edit the example profile as needed:
#   cp docs/examples/profile.oceanbase.v1.yaml ./profile-oceanbase.yaml

./mind-tpcc validate --profile ./profile-oceanbase.yaml
./mind-tpcc plan     --profile ./profile-oceanbase.yaml

# Full pipeline:
# validate → deploy → schema → load → check(after-import)
# → start → check(after-run) → collect → consolidate
./mind-tpcc run --profile ./profile-oceanbase.yaml
```

Or run stages individually: `deploy`, `schema`, `load`,
`check --after-import`, `start`, `check --after-run`, `collect`,
`consolidate`.

Artifacts land under `paths.result_root/<run_id>/` (including
`aggregate.json`, `orchestrator/run-config.json`, and
`profile.redacted.yaml`).

On a single host, set every loader/worker `host` to `127.0.0.1` (local
sessions, no SSH). Multi-host runs need SSH access and tightly synchronized
clocks.

#### Multiple workers / co-location

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
  threads_per_worker: 4
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
tpcc-oceanbase worker --run-config run-config.json --instance <worker> --start-at=<UTC>
tpcc-oceanbase check  --run-config run-config.json --instance check-0 --after-import|--after-run
```

### Checklist

1. OceanBase is reachable on the MySQL SQL port (default 2881).
2. `tpcc-oceanbase` is built (and `mind-tpcc` for orchestration).
3. Credentials are supplied (`--connection=...` or `TPCC_PASSWORD`;
   optional `database.user` / `TPCC_OB_USER`).
4. Binary is under `paths.local_artifacts` as `tpcc-oceanbase`.
5. Flow: `schema` → `import`/`load` → `check --after-import` → `run`/`start`
   → `check --after-run`.

For a quick engineering smoke test, standalone with `-w 10` and a short
`--duration` is enough. For settings closer to TPC-C 5.11, see the defaults
embedded in `mind-tpcc` and
[docs/tpcc-5.11-conformance-analysis.md](docs/tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).
