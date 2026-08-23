# Running TPC-C against YDB

Binary: `tpcc-ydb`. Full parameter list:
[parameter-reference.md](parameter-reference.md).

Two modes:

1. **Standalone** — drive `tpcc-ydb` directly (simplest for local smoke tests).
2. **Orchestrated** — use `mind-tpcc` with a YAML profile (multi-host and full
   pipeline).

Results MUST NOT be called official TPC-C results without the required TPC
verification.

## Build

The YDB binary **must** be built with CUDA disabled. From the repository root
(release shown; drop `-r` for the ya default/debug configuration):

```bash
./ya make -r -DHAVE_CUDA=no -DCUDA_VERSION=11.4 tpcc/app/ydb
go -C mind build ./cmd/mind-tpcc
```

Or build everything with `./build.sh` (it always passes those CUDA defines).
Do not omit them: the ya make graph otherwise expects a CUDA toolchain that is
not part of the portable-tpcc development setup.

Binaries:

| Binary | Path after build |
| --- | --- |
| `tpcc-ydb` | `tpcc/app/ydb/tpcc-ydb` |
| `mind-tpcc` | `mind/mind-tpcc` |

For orchestration, copy `tpcc-ydb` into the profile's `paths.local_artifacts`
directory (for example `./dist/tpcc-ydb`). Re-run `mind-tpcc deploy` after
rebuilding; `run` does not auto-upload binaries.

## Prepare YDB

Provision a YDB database before the run (local `/local`, dedicated database, or
cloud). The adapter does not create the YDB database itself. TPC-C tables are
created under `database` + `path` (table path prefix inside the database).

Default standalone endpoint is `localhost:2136`, database `/local`, path
`tpcc` (anonymous auth). Endpoint forms: `host:port`, `grpc://host:port`, or
`grpcs://host:port`. TLS is enabled for `grpcs://` and whenever `--ca-file` /
`database.ca_file` is set.

Do not put passwords, tokens, or key material in profile YAML/JSON.

## YDB-specific settings

YDB has no `database.options` keys. Unknown options are rejected at worker
startup. Warehouse-leading keys and range partitions (`warehouse_range`) are
chosen automatically from the warehouse scale at schema time.

| Standalone | Profile | Meaning |
| --- | --- | --- |
| `--endpoint` | `database.endpoint` | `host:port` / `grpc://` / `grpcs://`. Default `localhost:2136`. |
| `--database` | `database.database` | YDB database path (default `/local`). Must already exist. |
| `-p` / `--path` | `database.path` | Table path prefix inside the database (default `tpcc`). |
| `--auth-scheme` | `database.auth_scheme` | See [Authentication](#authentication). |
| `--user` | `database.user` | Login user (`auth_scheme=login`). Required for login. |
| `--password-env` | `database.password_env` | Env var with login password (not the secret itself). |
| `--token` / `--token-env` | — | Standalone token auth only. Not accepted in the profile. |
| `--sa-key-file` | `database.sa_key_file` | Service-account JSON key (`auth_scheme=sa_key`). |
| `--ca-file` | `database.ca_file` | PEM CA bundle for TLS. Orchestrator delivers it as `ca.pem`. |

`database.dbms` must be `ydb`.

### Authentication

Scheme is explicit or inferred:

| Scheme | When inferred | Credentials |
| --- | --- | --- |
| `anonymous` | no login/sa_key/token fields | none. Incompatible with `user`, `password_env`, `sa_key_file`. |
| `login` | `user` or `password_env` set | `user` + password via `password_env` (orchestrated: delivered as `db-password`). |
| `sa_key` | `sa_key_file` set | JSON key file. Incompatible with `user` / `password_env`. Orchestrator delivers it as `sa-key.json`. |
| `token` | `--token` or `--token-env` set | **Standalone only.** Orchestrated profiles reject token auth. |

Orchestrated `auth_scheme` must be `anonymous`, `login`, or `sa_key`.
`password_env` is required for `login` and must be an environment variable
name matching `[A-Za-z_][A-Za-z0-9_]*`, not a secret literal.

## Standalone local run

Anonymous local database:

```bash
BIN=./tpcc/app/ydb/tpcc-ydb

$BIN schema --endpoint=localhost:2136 --database=/local --path=tpcc -w 10
$BIN import --endpoint=localhost:2136 --database=/local --path=tpcc -w 10 -t 8
$BIN indexes --endpoint=localhost:2136 --database=/local --path=tpcc
$BIN check --endpoint=localhost:2136 --database=/local --path=tpcc -w 10 -t 10 --after-import
$BIN run --endpoint=localhost:2136 --database=/local --path=tpcc -w 10 \
  --duration=5 -t 4
$BIN check --endpoint=localhost:2136 --database=/local --path=tpcc -w 10 -t 10 --after-test
$BIN clean --endpoint=localhost:2136 --database=/local --path=tpcc
```

Login (password stays in the environment, not in argv):

```bash
export YDB_PASSWORD='...'
$BIN schema --endpoint=grpcs://ydb.example.net:2135 --database=/Root/tpcc \
  --path=portable_tpcc --auth-scheme=login --user=root \
  --password-env=YDB_PASSWORD --ca-file=./certs/ydb-ca.pem -w 10
```

Service-account key:

```bash
$BIN schema --endpoint=grpcs://ydb.example.net:2135 --database=/Root/tpcc \
  --path=portable_tpcc --auth-scheme=sa_key --sa-key-file=./secrets/sa-key.json \
  --ca-file=./certs/ydb-ca.pem -w 10
```

Token (standalone only; prefer `--token-env`):

```bash
export YDB_TOKEN='...'
$BIN schema --endpoint=grpcs://ydb.example.net:2135 --database=/Root/tpcc \
  --path=tpcc --auth-scheme=token --token-env=YDB_TOKEN -w 10
```

Useful shared flags: `--no-delays`, `--help`.
See [parameter-reference.md](parameter-reference.md#standalone-tpcc-dbms-cli).

Worker `ITpccTransaction` does not block the scheduler on YDB SDK IO. Paced
runs can keep `--threads` / `threads_per_worker` low (including auto
`threads=0`) and a large `--max_inflight` / `max_inflight_per_worker`; progress
`Inflight` should exceed `ThreadCount` when the database has headroom. See
[async-adapter-transactions.md](async-adapter-transactions.md).

`indexes` creates secondary indexes after load (YDB has no PostgreSQL-style
`ANALYZE` step).

## Orchestrated run (`mind-tpcc`)

Minimum profile fields: `apiVersion` / `kind` / `metadata.name`, `ssh`,
`paths`, `database`, `scale.warehouses`, non-empty `loaders` and `workers`,
and `phases` durations. Omitted workload fields use built-in TPC-C 5.11-style
defaults. Full schema: [parameter-reference.md](parameter-reference.md#profile-yaml).

A ready example lives in
[`docs/examples/profile.ydb.v1.yaml`](examples/profile.ydb.v1.yaml):

```yaml
database:
  dbms: ydb
  endpoint: grpcs://ydb.example.net:2135
  database: /Root/tpcc
  path: portable_tpcc
  auth_scheme: login          # anonymous | login | sa_key
  user: root                  # required for login
  password_env: YDB_PASSWORD  # required for login
  # ca_file: ./certs/ydb-ca.pem
  # auth_scheme: sa_key
  # sa_key_file: ./secrets/sa-key.json

scale:
  warehouses: 200

loaders:
  - 10.10.0.21
  - 10.10.0.22

workers:
  - 10.10.0.31
  - 10.10.0.32

phases:
  start_lead: 45s
  ramp_up: 5m
  measurement: 30m
  transaction_drain: 30s
  stop_grace: 15s
  max_clock_skew_ms: 100

checks:
  after_import: true
  after_test: true
```

For a single-host smoke test, list `127.0.0.1` for every loader/worker and a
small `scale.warehouses`. Anonymous local auth:

```yaml
database:
  dbms: ydb
  endpoint: localhost:2136
  database: /local
  path: tpcc
  auth_scheme: anonymous
```

```bash
export YDB_PASSWORD='...'   # login only

mkdir -p dist
cp tpcc/app/ydb/tpcc-ydb dist/
cp mind/mind-tpcc ./mind-tpcc

# Generate a complete starter profile, or copy the lab example:
# ./mind-tpcc configure --profile ./profile-ydb.yaml --dbms ydb
# cp docs/examples/profile.ydb.v1.yaml ./profile-ydb.yaml

./mind-tpcc validate --profile ./profile-ydb.yaml
./mind-tpcc plan     --profile ./profile-ydb.yaml
./mind-tpcc deploy --profile ./profile-ydb.yaml
./mind-tpcc run --profile ./profile-ydb.yaml
```

Or run stages individually: `deploy`, `schema`, `load`, `indexes`,
`check --after-import`, `test`, `check --after-test`, `collect`,
`consolidate`. After `test`, `consolidate` is enough to produce the result:
it runs `collect` first when `collection-manifest.json` is absent.

`mind-tpcc run` includes `check(after-import)` / `check(after-test)` only when
`checks.after_import` / `checks.after_test` are true.

On a single host, list `127.0.0.1` for every loader/worker (local
sessions, no SSH). Multi-host runs need SSH access and tightly synchronized
clocks. Repeated host strings mean co-location on one machine.

Orchestrated roles launched by mind (for reference):

```text
tpcc-ydb schema --run-config run-config.json --instance schema-0
tpcc-ydb loader --run-config run-config.json --instance <loader> [--threads=N]
tpcc-ydb indexes --run-config run-config.json --instance indexes-0
tpcc-ydb worker --run-config run-config.json --instance <worker> --start-at=<UTC> [--threads=N]
tpcc-ydb check  --run-config run-config.json --instance check-0 --after-import|--after-test [--threads=N]
tpcc-ydb clean  --run-config run-config.json --instance clean-0   # mind-tpcc cleanup
```

Artifacts land under `paths.result_root/<run_id>/` (including
`aggregate.json`, `orchestrator/run-config.json`, and
`profile.redacted.yaml`).

```bash
./mind-tpcc cleanup --profile ./profile-ydb.yaml --yes
./mind-tpcc undeploy --profile ./profile-ydb.yaml --yes
```

## Checklist

1. YDB is reachable; the database path already exists.
2. `tpcc-ydb` is built with `-DHAVE_CUDA=no -DCUDA_VERSION=11.4` (and
   `mind-tpcc` for orchestration).
3. Auth matches the scheme (anonymous, or `YDB_PASSWORD` / `sa_key_file`, or
   standalone `--token-env`).
4. Binary is under `paths.local_artifacts` as `tpcc-ydb` for orchestration.
5. Flow: `schema` → `import`/`load` → `indexes` → `check --after-import`
   → `run`/`test` → `check --after-test`.

For a quick engineering smoke test, standalone with `-w 10` and a short
`--duration` is enough. For settings closer to TPC-C 5.11, see the defaults
embedded in `mind-tpcc` and
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).
