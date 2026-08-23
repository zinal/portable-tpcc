# Running TPC-C against PostgreSQL

Binary: `tpcc-pgsql`. Full parameter list:
[parameter-reference.md](parameter-reference.md).

Two modes:

1. **Standalone** — drive `tpcc-pgsql` directly (simplest for local smoke tests).
2. **Orchestrated** — use `mind-tpcc` with a YAML profile (multi-host and full
   pipeline).

Results MUST NOT be called official TPC-C results without the required TPC
verification.

## Build

From the repository root:

```bash
./ya make tpcc/app/pgsql
go -C mind build ./cmd/mind-tpcc
```

Or build everything with `./build.sh` (it always passes
`-DHAVE_CUDA=no -DCUDA_VERSION=11.4`, required when the YDB target is in the
graph).

Binaries:

| Binary | Path after build |
| --- | --- |
| `tpcc-pgsql` | `tpcc/app/pgsql/tpcc-pgsql` |
| `mind-tpcc` | `mind/mind-tpcc` |

For orchestration, copy `tpcc-pgsql` into the profile's `paths.local_artifacts`
directory (default `.`, the current directory). Re-run `mind-tpcc deploy` after
rebuilding; `run` does not auto-upload binaries.

## Prepare PostgreSQL

```bash
createdb tpcc
```

The database must already exist. `tpcc-pgsql schema` creates the TPC-C schema
(`CREATE SCHEMA IF NOT EXISTS` when `--path` / `database.path` is set) and
tables inside it. Empty `--path` uses the server `search_path` (effective
schema `public`).

Do not put passwords in profile YAML/JSON. Pass them via the connection string
(standalone) or an environment variable named in `password_env` on the control
host (orchestrated). `mind-tpcc` copies that value to a mode-0600 `db-password`
file on each worker and points `run-config.json` at `password_file`.

Client user: profile `database.user`, else `postgres`.

## PostgreSQL-specific settings

| Standalone | Profile | Meaning |
| --- | --- | --- |
| `--connection` | `database.endpoint` + `database` + `user` + `password_env` | libpq string vs orchestrated `host` or `host:port` (default port **5432**). Endpoint must not contain `user=` / `password=`. |
| `-p` / `--path` | `database.path` | PostgreSQL schema for TPC-C tables. Empty → server `search_path` (`public`). |
| — | `database.dbms: pgsql` | Required in the profile. |
| — | `database.user` | PostgreSQL login role. Default `postgres` when omitted. |
| `--partitioning` | `database.options.partitioning` | `none` (default) or `warehouse_hash`. |
| `--partition-count` | `database.options.partition_count` | HASH modulus when `warehouse_hash`. Standalone `0` or omitted profile key → derive from `-w` / `scale.warehouses`. Profile value, if set, must be a positive integer (max **1024**). |
| `--foreign_keys` | `database.options.foreign_keys` | `on` (default) or `off`. |

`--partitioning=warehouse_hash` HASH-partitions warehouse-scoped tables
(`stock`, `customer`, `history`, `oorder`, `new_order`, `order_line`) by
warehouse id. `warehouse` / `district` / `item` stay unpartitioned. Design:
[pgsql-partitioning-design.md](pgsql-partitioning-design.md).

`--foreign_keys=off` omits FOREIGN KEY constraints at schema time. Idempotent
load still replaces warehouse ranges via explicit deletes.

Unknown `database.options.*` keys are rejected for `dbms=pgsql`.

## Standalone local run

```bash
CONN='host=localhost port=5432 dbname=tpcc user=postgres password=YOUR_PASSWORD'
BIN=./tpcc/app/pgsql/tpcc-pgsql

# schema (unpartitioned; --foreign_keys=off omits FKs, default is on)
$BIN schema --connection="$CONN" --path=portable_tpcc -w 10 \
  --foreign_keys=off

# schema with HASH partitions by warehouse id (also without FKs)
$BIN schema --connection="$CONN" --path=portable_tpcc -w 10 \
  --partitioning=warehouse_hash --foreign_keys=off
# optional explicit modulus (otherwise derived from -w):
#   --partition-count=64

# load
$BIN import --connection="$CONN" --path=portable_tpcc -w 10 -t 8

# indexes + ANALYZE (after load)
$BIN indexes --connection="$CONN" --path=portable_tpcc

# check after load
$BIN check --connection="$CONN" --path=portable_tpcc -w 10 -t 10 --after-import

# measurement run (durations in minutes)
$BIN run --connection="$CONN" --path=portable_tpcc -w 10 \
  --duration=5 -t 4

# check after run
$BIN check --connection="$CONN" --path=portable_tpcc -w 10 -t 10 --after-test

# drop TPC-C tables
$BIN drop --connection="$CONN" --path=portable_tpcc
```

Useful shared flags: `--no-delays` (disable keying/think time), `--help`.
See [parameter-reference.md](parameter-reference.md#standalone-tpcc-dbms-cli).

Worker `ITpccTransaction` does not block the scheduler on libpqxx IO. Paced
runs can keep `--threads` / `threads_per_worker` low (including auto
`threads=0`) and a large `--max_inflight` / `max_inflight_per_worker`; progress
`Inflight` should exceed `ThreadCount` when the database has headroom. See
[async-adapter-transactions.md](async-adapter-transactions.md).

## Orchestrated run (`mind-tpcc`)

Minimum profile fields: `apiVersion` / `kind` / `metadata.name`, `ssh`,
`paths`, `database`, `scale.warehouses`, non-empty `loaders` and `workers`,
and `phases` durations. Omitted workload fields use built-in TPC-C 5.11-style
defaults. Full schema: [parameter-reference.md](parameter-reference.md#profile-yaml).

Use `database.dbms: pgsql`. Generate a complete starter profile (every field
set to the built-in default, hosts `localhost`):

```bash
./mind-tpcc configure --profile ./profile-pgsql.yaml --dbms pgsql
```

A minimal hand-written example lives in
[`mind/testdata/profile.valid.yaml`](../mind/testdata/profile.valid.yaml):

```yaml
database:
  dbms: pgsql
  endpoint: localhost:5432          # host or host:port; no user=/password=
  database: tpcc
  path: portable_tpcc
  user: postgres                    # optional; default postgres
  password_env: TPCC_PASSWORD
  options:
    partitioning: warehouse_hash    # omit or "none" for unpartitioned tables
    # partition_count: 64           # optional; else derived from scale.warehouses
    foreign_keys: off               # omit FKs at schema time; default on

scale:
  warehouses: 10

loaders:
  - 127.0.0.1

workers:
  - 127.0.0.1

phases:
  start_lead: 45s
  ramp_up: 1m
  measurement: 5m
  transaction_drain: 30s
  stop_grace: 15s
  max_clock_skew_ms: 100

# Integrity checks in `mind-tpcc run` only run when enabled:
checks:
  after_import: true
  after_test: true
```

```bash
export TPCC_PASSWORD='...'

cp tpcc/app/pgsql/tpcc-pgsql .
cp mind/mind-tpcc ./mind-tpcc

./mind-tpcc validate --profile ./profile-pgsql.yaml
./mind-tpcc plan     --profile ./profile-pgsql.yaml

# Explicit deploy installs the shared worker binary under paths.remote_root.
# Re-run deploy after rebuilding tpcc-*; `run` will not auto-upload binaries.
./mind-tpcc deploy --profile ./profile-pgsql.yaml

# Full pipeline (requires prior deploy):
# validate → require deploy → schema → load → indexes
# → check(after-import) if checks.after_import → test
# → check(after-test) if checks.after_test → collect → consolidate
./mind-tpcc run --profile ./profile-pgsql.yaml
```

Or run stages individually: `deploy`, `schema`, `load`, `indexes`,
`check --after-import`, `test`, `check --after-test`, `collect`,
`consolidate`. After `test`, `consolidate` is enough to produce the result:
it runs `collect` first when `collection-manifest.json` is absent.

On a single host, list `127.0.0.1` for every loader/worker (local
sessions, no SSH). Multi-host runs need SSH access and tightly synchronized
clocks. Repeated host strings mean co-location on one machine.

Artifacts land under `paths.result_root/<run_id>/` (including
`aggregate.json`).

```bash
./mind-tpcc drop --profile ./profile-pgsql.yaml --yes
./mind-tpcc cleanup --profile ./profile-pgsql.yaml --yes
./mind-tpcc undeploy --profile ./profile-pgsql.yaml --yes
```

`drop` removes TPC-C objects. `cleanup` removes remote `remote_root/<run_id>`
and local results + state (including the control host). Shared worker binaries
stay installed until `undeploy`.

## Checklist

1. PostgreSQL is reachable and the database exists (`createdb`).
2. `tpcc-pgsql` is built (and `mind-tpcc` for orchestration).
3. Credentials are supplied (`--connection=...` or `TPCC_PASSWORD`).
4. Flow: `schema` → `import`/`load` → `indexes` → `check --after-import`
   → `run`/`test` → `check --after-test`.

For a quick engineering smoke test, standalone with `-w 10` and a short
`--duration` is enough. For settings closer to TPC-C 5.11, see the defaults
embedded in `mind-tpcc` and
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).
