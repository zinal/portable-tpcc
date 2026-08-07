# portable-tpcc

A horizontally scalable TPC-C implementation with shared workload logic,
YDB/PostgreSQL/OceanBase adapters, and a dedicated orchestrator.

Architecture draft:

- [specification](docs/specification.md);
- [shared libraries and adapter API](docs/adapter-api.md);
- [profile example](docs/examples/profile.v1.yaml);
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
2. **Orchestrated** — use `tpccctl` with a YAML profile (multi-host and full
   pipeline).

### Build

From the repository root:

```bash
./ya make tpcc/app/pgsql
go -C tpccctl build ./cmd/tpccctl
```

Or build everything with `./build.sh`. The worker/loader binary is
`tpcc-pgsql`. For orchestration, place it under the profile's
`paths.local_artifacts` directory (for example `./dist/tpcc-pgsql`).

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

# schema (unpartitioned by default)
./tpcc-pgsql schema --connection="$CONN" --path=portable_tpcc -w 10

# schema with HASH partitions by warehouse id
./tpcc-pgsql schema --connection="$CONN" --path=portable_tpcc -w 10 \
  --partitioning=warehouse_hash
# optional explicit modulus (otherwise derived from -w):
#   --partition-count=64

# schema without FOREIGN KEY constraints (faster load; default is on)
./tpcc-pgsql schema --connection="$CONN" --path=portable_tpcc -w 10 \
  --foreign_keys=off

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

### Orchestrated run (`tpccctl`)

Use a profile with `database.dbms: pgsql`. A minimal example lives in
[`tpccctl/testdata/profile.valid.yaml`](tpccctl/testdata/profile.valid.yaml):

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
    # foreign_keys: off             # optional; default on
```

For PostgreSQL, `database.user` is not set in the profile. The client user
defaults to `postgres`, or to the value of `TPCC_PG_USER` when that env var
is set.

```bash
export TPCC_PASSWORD='...'
# export TPCC_PG_USER=myuser   # if not postgres

mkdir -p dist && cp /path/to/tpcc-pgsql dist/

go -C tpccctl run ./cmd/tpccctl validate --profile ./profile-pgsql.yaml
go -C tpccctl run ./cmd/tpccctl plan     --profile ./profile-pgsql.yaml

# Full pipeline:
# validate → deploy → schema → load → check(after-import)
# → start → check(after-run) → collect → consolidate
go -C tpccctl run ./cmd/tpccctl run --profile ./profile-pgsql.yaml
```

Or run stages individually: `deploy`, `schema`, `load`,
`check --after-import`, `start`, `check --after-run`, `collect`,
`consolidate`.

Artifacts land under `paths.result_root/<run_id>/` (including
`aggregate.json`).

On a single host, every `hosts.*.address` may be `127.0.0.1`. Multi-host
runs need SSH access and tightly synchronized clocks.

### Checklist

1. PostgreSQL is reachable and the database exists.
2. `tpcc-pgsql` is built (and `tpccctl` for orchestration).
3. Credentials are supplied (`--connection=...` or `TPCC_PASSWORD`).
4. Flow: `schema` → `import`/`load` → `check --after-import` → `run`/`start`
   → `check --after-run`.

For a quick engineering smoke test, standalone with `-w 10` and a short
`--duration` is enough. For settings closer to TPC-C 5.11, see the defaults
embedded in `tpccctl` and
[docs/tpcc-5.11-conformance-analysis.md](docs/tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).
