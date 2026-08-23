# Parameter reference

Canonical list of portable-tpcc run parameters. Run guides:

- [PostgreSQL](run-pgsql.md)
- [YDB](run-ydb.md)
- [OceanBase](run-oceanbase.md)

Profile YAML is `portable-tpcc/v1` (`kind: TpccRunProfile`). Unknown fields are
rejected. Omitted workload fields use defaults embedded in `mind-tpcc`.
`mind-tpcc` materializes a secret-free `run-config.json` for workers.

Results MUST NOT be called official TPC-C results without the required TPC
verification.

## mind-tpcc CLI

```text
mind-tpcc <command> --profile <path> [options]
mind-tpcc configure --profile <path> --dbms <pgsql|ydb|oceanbase> [options]
```

### Commands

| Command | Purpose |
| --- | --- |
| `configure` | Write a complete example profile YAML. Requires `--profile` (or a positional path) and `--dbms`. Omitted fields use built-in defaults; host lists default to `localhost`. |
| `validate` | Validate the profile; print JSON (structural errors fail; TPC-C setting deviations are informational). |
| `plan` | Show planned warehouse assignment and argv. |
| `deploy` | Install the shared worker binary under `paths.remote_root` (profile-scoped; no `run_id`). |
| `undeploy` | Remove that shared binary. Requires `--yes`. |
| `schema` | Create TPC-C schema. |
| `load` | Horizontal data load. |
| `indexes` | Secondary indexes (and DBMS stats where the adapter supports them). |
| `check` | Integrity checks. Requires `--after-import` or `--after-test`. Available after a completed load (indexes reached or skipped); does not wait for `test`. |
| `test` | Arm workers and run ramp-up / measurement / drain. `start` is a compatibility alias. |
| `status` | Show run state. |
| `stop` | Stop workers gracefully. |
| `collect` | Copy artifacts from runtime hosts. |
| `consolidate` | Merge worker results into `aggregate.json` and print a brief stats summary. Runs `collect` first when `collection-manifest.json` is absent. |
| `run` | Full pipeline. Requires a prior explicit `deploy`. |
| `cleanup` | Teardown: stop, optional DB `clean`, remote + local run artifacts. Requires `--yes`. |
| `help` / `-h` / `--help` | Usage. |

`run` = validate → require prior `deploy` → schema → load → indexes →
check(after-import) **if** `checks.after_import` → test →
check(after-test) **if** `checks.after_test` → collect → consolidate.

`run` does not upload binaries. Re-run `deploy` after rebuilding `tpcc-*`.

`cleanup --yes` uses `--run-id` if given, otherwise the newest matching run.
When state is past deploy it launches orchestrated `clean` on the first loader
host; when past planned it removes `remote_root/<run_id>` on every runtime
host; it always removes `result_root/<run_id>` and `state/runs/<run_id>`.
Shared binaries stay until `undeploy`.

### Flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--profile <path>` | required | Profile YAML. |
| `--run-id <id>` | latest active run for this profile, else allocate | Run identifier. |
| `--worker-binary <path>` | `tpcc-<dbms>` from `paths.local_artifacts` | Worker binary to deploy. Basename is stored in run-config. |
| `--warehouses <n>` | profile `scale.warehouses` | Override; must be positive and **≤** profile value. Cannot disagree with an already materialized run-config. |
| `--ramp-up <duration>` | profile `phases.ramp_up` | Warmup override (`30s`, `5m`, …). |
| `--measurement <duration>` | profile `phases.measurement` | Measurement override. |
| `--threads <n>` | profile worker/loader threads and `runtime.check_concurrency` | Launch-time override for this invocation. `test`/`load`/`run` pass `--threads=N` to workers and loaders (`0` = auto at the binary). `check`/`run` pass a resolved session count to `check` (`0` = auto `min(scale.warehouses, 32)`). Does not rewrite an existing run-config. |
| `--skip <step>` | none | Skip a `run` pipeline step. Repeatable. Names: `deploy`, `schema`, `load`, `indexes`, `check_after_import`, `test` (alias `start`), `check_after_test` (alias `check_after_run`), `collect`, `consolidate`. |
| `--yes` | false | Required for `cleanup` and `undeploy`. `configure` uses it to overwrite an existing file. |
| `--after-import` / `--after-test` | — | Select the `check` phase. `--after-run` is a deprecated alias for `--after-test`. |
| `--leave-processes` | false | Debug: leave remote processes running when `mind-tpcc` exits. Default is to stop leftovers this invocation launched (and warn if a process is still alive after it reported finished). |

Unknown flags and extra positional arguments fail the invocation (exit 2).

### `configure`

Writes a secret-free example `TpccRunProfile` with every current field set
to the built-in default for `--dbms` (PostgreSQL / YDB / OceanBase options
included; YDB `anonymous` omits login / `sa_key` fields). `metadata.name`
defaults to the sanitized filename. `ssh.user` defaults to the current
account. Loader and worker lists default to a single `localhost` entry.
`paths` defaults: `local_artifacts: .`, `remote_root: portable-tpcc`,
`result_root: results`, `state_dir: state`.

```text
mind-tpcc configure --profile ./profile.yaml --dbms pgsql
mind-tpcc configure ./profile.yaml --dbms ydb --warehouses 50 --endpoint localhost:2136
```

Optional flags override the corresponding profile fields (`--name`,
`--ssh-user`, `--endpoint`, `--database`, `--path`, `--user`,
`--password-env`, `--warehouses`, `--seed`, `--loaders`, `--workers`,
phase durations, runtime / retry / histogram / checks / collect knobs,
and DBMS-specific `--auth-scheme`, `--partitioning`, `--partitions`,
`--foreign-keys`, `--query-timeout`, `--index-parallel`, …). See
`mind-tpcc configure --help`. The generated file is rejected if the
result would not pass structural `validate`.

## Profile YAML

```yaml
apiVersion: portable-tpcc/v1   # required
kind: TpccRunProfile           # required
```

Obsolete top-level fields `mode`, `spec`, `deviations` are rejected. Manual
assignment fields `warehouse_ranges`, `assignment`, `owns_global_data` are
prohibited (mind computes them).

Examples: [profile.ydb.v1.yaml](examples/profile.ydb.v1.yaml),
[profile.oceanbase.v1.yaml](examples/profile.oceanbase.v1.yaml),
[mind/testdata/profile.valid.yaml](../mind/testdata/profile.valid.yaml).

### `metadata`

| Field | Required | Meaning |
| --- | --- | --- |
| `name` | yes | Profile name. Used in generated `run_id` (`YYYYMMDD-<name>-01`). |

### `ssh`

Required even for loopback (`127.0.0.1`) profiles.

| Field | Required | Default | Meaning |
| --- | --- | --- | --- |
| `user` | yes | — | SSH / remote account. |
| `use_agent` | no | `false` | Use `SSH_AUTH_SOCK`. |
| `known_hosts` | unless `insecure_ignore_host_key` | — | Host-key file (`~` expanded on the control host). |
| `connect_timeout` | no | — | Go duration, e.g. `10s`. |
| `insecure_ignore_host_key` | no | `false` | Disable host-key checking (recorded in run-state). |

### `paths`

Required in a hand-written profile. `mind-tpcc configure` fills omitted
values with the defaults below.

| Field | Required | Default | Meaning |
| --- | --- | --- | --- |
| `local_artifacts` | yes | `.` | Control-host directory that contains `tpcc-<dbms>` (`~` expanded locally). |
| `remote_root` | yes | `portable-tpcc` | Runtime-host directory for the shared binary and per-run dirs. Interpreted on each runtime host (SSH cwd/home). Do not use a control-host-only absolute path. |
| `result_root` | yes | `results` | Control-host results directory. Artifacts: `result_root/<run_id>/`. |
| `state_dir` | yes | `state` | Control-host orchestrator state. |

### `database` (common)

| Field | DBMS | Required | Meaning |
| --- | --- | --- | --- |
| `dbms` | all | yes | `pgsql` \| `ydb` \| `oceanbase`. |
| `endpoint` | all | yes | Host or `host:port` (YDB also `grpc://` / `grpcs://`). Must not contain `user=` / `password=`. |
| `database` | all | yes | DBMS database name/path (`dbname`, YDB path, OceanBase `database=`). |
| `path` | all | recommended | TPC-C object location (PG schema, YDB table prefix, OceanBase tables database). |
| `password_env` | pgsql, oceanbase; ydb login | see DBMS | Control-host **env var name** (not the secret). Pattern `[A-Za-z_][A-Za-z0-9_]*`. |
| `user` | pgsql, ydb, oceanbase | see DBMS | PostgreSQL role (default `postgres`), YDB login user, or OceanBase `user@tenant`. |
| `auth_scheme` | ydb | no | `anonymous` \| `login` \| `sa_key`. Inferred if omitted. |
| `sa_key_file` | ydb | for `sa_key` | Service-account JSON on the control host. Delivered as `sa-key.json`. |
| `ca_file` | ydb | no | PEM CA bundle. Delivered as `ca.pem`. |
| `options` | pgsql, oceanbase | no | Adapter options. YDB accepts none. |

Default ports when `endpoint` has no port: PostgreSQL **5432**, OceanBase
**2881**. YDB standalone default endpoint is `localhost:2136` (no implicit
port in the profile).

Secrets: `mind-tpcc` reads `password_env` on the control host and writes
mode-0600 `db-password` beside `run-config.json`. The secret is not placed in
argv, SSH/`nohup` command lines, stored profiles, or logs.

#### PostgreSQL `database.options`

| Key | Values | Default | Meaning |
| --- | --- | --- | --- |
| `partitioning` | `none` \| `warehouse_hash` | `none` | HASH by warehouse id on warehouse-scoped tables. |
| `partition_count` | positive int, max **1024** | derived from `scale.warehouses` | HASH modulus. Only valid with `warehouse_hash`. |
| `foreign_keys` | bool or `on`/`off`/`true`/`false`/`1`/`0` | `on` | FOREIGN KEY constraints at schema time. |

See [pgsql-partitioning-design.md](pgsql-partitioning-design.md).
PostgreSQL user: `database.user`, else `postgres`.

#### YDB authentication

| `auth_scheme` | Inferred when | Fields |
| --- | --- | --- |
| `anonymous` | no user / password / sa_key | No `user`, `password_env`, or `sa_key_file`. |
| `login` | `user` or `password_env` set | `user` required; `password_env` required. No `sa_key_file`. |
| `sa_key` | `sa_key_file` set | `sa_key_file` required. No `user` / `password_env`. |

Standalone `tpcc-ydb` also supports `token` / `--token` / `--token-env`.
Orchestrated profiles do not.

YDB `database.options` must be empty (or omitted). Range partitioning is
automatic from warehouse scale (`warehouse_range`).

#### OceanBase `database.options`

| Key | Values | Default | Meaning |
| --- | --- | --- | --- |
| `partitions` | `-1`, `0`, or `1`…`8192` | `0` | `-1` off, `0` derive from warehouses, `N` explicit HASH count (schema time only). |
| `foreign_keys` | bool or `on`/`off`/… | `on` | FOREIGN KEY constraints at schema time. |
| `query_timeout` | positive int (seconds) | `600` | Session `ob_query_timeout` for bulk import / `CREATE INDEX` / `DBMS_STATS` / integrity checks. |
| `index_parallel` | positive int | `4` | `CREATE INDEX … PARALLEL n`. `1` = serial. |

OceanBase user: `database.user`, else `TPCC_OB_USER`, else `root@root`.
Schema creates the tables database if missing.

### `scale`

| Field | Required | Meaning |
| --- | --- | --- |
| `warehouses` | yes | Positive warehouse count. Must be ≥ loader count and ≥ worker count. |

Assignment algorithm `balanced-contiguous`: sort generated instance names
bytewise; split warehouses into contiguous ranges; remainder to the first
instances; DB-wide data belongs to the first loader. A warehouse's home
terminals are never split across workers.

### `data`

| Field | Default | Meaning |
| --- | --- | --- |
| `seed` | omitted → loader default **1** | Deterministic generator seed. Set explicitly for cross-DBMS comparison. |
| `batch_rows` | `10000` if ≤ 0 | Bulk-load batch size. Must not be negative. |

### `workload`

All fields optional; zeros/omissions keep the built-in default.

| Field | Default | TPC-C 5.11 |
| --- | --- | --- |
| `terminals_per_warehouse` | `10` | must be 10 (values > 10 also break Stock-Level uniqueness) |
| `transaction_mix.new_order` | `45` | no Clause 5.2.3 minimum (typical remainder after other minima) |
| `transaction_mix.payment` | `43` | ≥ 43% |
| `transaction_mix.order_status` | `4` | ≥ 4% |
| `transaction_mix.delivery` | `4` | ≥ 4% |
| `transaction_mix.stock_level` | `4` | ≥ 4% |
| `keying_time_ms.*` | 18000 / 3000 / 2000 / 2000 / 2000 | minima (ms); larger values remain TPC-C conformant |
| `think_time_ms.*` | 12000 / 12000 / 10000 / 5000 / 5000 | minimum means (ms); larger values remain TPC-C conformant |

Mix weights must all be positive. Percentages are weight/sum. Deviations from
TPC-C 5.11 are reported by `validate` / `test` / `aggregate` and do **not**
fail structural validation.

### `loaders` / `workers`

Non-empty lists of host addresses (hostname, IP, or `host:port` for SSH).
Repeats are allowed and mean co-location (one SSH/local session per distinct
host string). Each entry is a scalar, not a `{name, host}` object.

Instance identities used in run-config, `--instance`, and remote directories
are generated as `{sanitized-host}-{n}` with a 1-based index over all loader
and worker entries that share that host (loaders first). Sanitization
lowercases the address, replaces non `[a-z0-9]` runs with `-`, and prefixes
`h-` when the result starts with a digit (`10.10.0.21` → `h-10-10-0-21-1`).

`127.0.0.1` uses local sessions (no SSH). Multi-host needs SSH and tightly
synchronized clocks.

### `phases`

Durations: Go duration (`45s`, `5m`, `100ms`) or a bare integer (milliseconds).
All listed fields except `async_work_drain` are required.

| Field | Meaning |
| --- | --- |
| `start_lead` | Wall-clock budget before `--start-at` (`start-at = now + start_lead`). |
| `ramp_up` | Warmup; samples are excluded from measurement metrics. |
| `measurement` | Measurement interval; must be **> 0**. TPC-C 5.11 requires ≥ **120 minutes** (shorter positive values are a soft deviation). |
| `transaction_drain` | Drain after measurement. |
| `async_work_drain` | Defaults to `transaction_drain`. No-op: adapters report `async_delivery = false`. |
| `stop_grace` | Grace period when stopping processes. |
| `max_clock_skew_ms` | Skew budget for status/validation (still a duration string, e.g. `100` or `100ms`). |

### `runtime`

| Field | Default | Meaning |
| --- | --- | --- |
| `pacing` | `enabled` | `enabled` \| `disabled`. TPC-C requires enabled (keying + think time). |
| `think_time_distribution` | `exponential` | `exponential` (TPC-C §5.2.5.4) \| `compatibility` \| `constant` (`constant` is an alias of `compatibility`: fixed mean think time). |
| `threads_per_loader` | `0` | Import concurrency. `0` = auto (min of assigned warehouses, host CPUs, adapter max). |
| `threads_per_worker` | `0` | Worker coroutine threads. `0` / omit keeps `threads: 0` in the assignment so each worker applies the same CPU + warehouse auto as standalone `--threads=0` / tpcc-postgres-cpp (see `ComputeRunLayout`, ≈ `ceil(warehouses / 1000)`). Explicit `N > 0` pins that many threads per worker. Auto sizing is useful when `ITpccTransaction` does not block the scheduler (PostgreSQL, OceanBase, and YDB worker paths). See [async-adapter-transactions.md](async-adapter-transactions.md). |
| `check_concurrency` | `0` | Parallel DBMS sessions for integrity checks. `0` / omit = auto (`min(scale.warehouses, 32)`). `1` = serial. Passed to `tpcc-<dbms> check` as `--threads=N`. `mind-tpcc --threads` overrides check concurrency, and also worker/loader threads, for the current invocation without rewriting run-config. |
| `max_inflight_per_worker` | `100` if ≤ 0 | Max in-flight transactions per worker. Matches standalone `tpcc-* --max_inflight` / tpcc-postgres-cpp default. Override when a shard needs a higher cap (also bounded by adapter `MaxRecommendedInflight`). |
| `retry.max_attempts` | `4` | Retry attempts. |
| `retry.initial_backoff` | `10ms` | Initial backoff. |
| `retry.max_backoff` | `500ms` | Max backoff (≥ initial). |
| `retry.jitter` | `full` | `full` \| `none`. |
| `histogram.unit` | `us` | Latency unit for `linear_exp` histograms. If set, MUST be `ms` or `us`. |
| `histogram.highest` | `120000000` | Histogram max value. If set, MUST be greater than zero; omitted uses the default. Worker derives `hdr_till` (default 4096, capped by `highest`). |

`retry_ambiguous_commit` is **not** a profile field. Run-config always
materializes `false` (no retry after an ambiguous commit). Do not add it to
YAML (`KnownFields` will reject it).

HDR-style `lowest` / `significant_figures` are rejected.

### `checks`

| Field | Default | Meaning |
| --- | --- | --- |
| `after_import` | `false` | Run post-load checks in `mind-tpcc run`. |
| `after_test` | `false` | Run post-test checks in `mind-tpcc run`. `after_run` is a deprecated alias. |
| `fail_fast` | `false` | If `false`, a failed check step is logged and `run` continues; if `true`, the run fails. |

Standalone / individual `mind-tpcc check` still need `--after-import` or
`--after-test` regardless of these flags.

### `collect`

| Field | Default | Meaning |
| --- | --- | --- |
| `include_events` | `false` | Accepted by the profile parser. Currently unused: collect always copies the standard artifact set (`result.json`, `ready.json`, `process.json`, `stdout.log`, `stderr.log`). |
| `include_logs` | `false` | Same: currently unused. |

## Environment variables

| Variable | Used by | Meaning |
| --- | --- | --- |
| name in `password_env` | `mind-tpcc`, workers | Password for PostgreSQL, OceanBase, or YDB login. |
| `TPCC_OB_USER` | `tpcc-oceanbase` | OceanBase user when `database.user` is omitted; default `root@root`. |
| `--password-env` / `--token-env` target | `tpcc-ydb` standalone | Login password or IAM/token string. |
| `SSH_AUTH_SOCK` | SSH | Used when `ssh.use_agent: true`. |

Never put secret literals in `password_env` (values containing `=` or `user:pass@host` are rejected).

## Standalone `tpcc-<dbms>` CLI

```text
tpcc-<dbms> <command> [options]
```

gflags accepts both `--flag-name` and `--flag_name`.

### Commands

Normative roles: `schema`, `loader`, `indexes`, `worker`, `check`.

Local aliases: `init` ≡ `schema`; `import` (standalone load); `run`
(standalone measurement); `clean` (drop TPC-C objects).

Orchestrated invocation (written by mind):

```text
schema  --run-config <path> --instance <name>
loader  --run-config <path> --instance <name> [--threads=N]
indexes --run-config <path> --instance <name>
worker  --run-config <path> --instance <name> --start-at=<RFC3339-UTC> [--threads=N]
check   --run-config <path> --instance <name> --after-import|--after-test [--threads=N]
clean   --run-config <path> --instance <name>
```

### Shared flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `-w` / `--warehouses` | `1` | Warehouse count (> 0). |
| `--seed` | `1` | Generator seed. |
| `--warmup` | `0` | Warmup minutes; `0` = adaptive. |
| `--skip-warmup` | `false` | Skip warmup; start measurement immediately. |
| `--duration` | `10` | Measurement minutes (> 0). |
| `-t` / `--threads` | `0` | Run/import: `0` = auto. Check: parallel DBMS sessions (`<=0` = 1 session). Orchestrated worker/loader: `mind-tpcc --threads` when set, otherwise assignment `threads` from run-config. Orchestrated check: `mind-tpcc --threads` when set, otherwise `runtime.check_concurrency`. |
| `-m` / `--max-inflight` | `100` | Max in-flight transactions (> 0). |
| `--no-delays` | `false` | Disable keying and think time (engineering). |
| `--think-time-distribution` | `exponential` | `exponential` \| `compatibility` \| `constant`. |
| `--high-res-histogram` | `false` | High-resolution histograms. |
| `--simulate-select1` | `0` | If > 0, run N `SELECT 1` probes per transaction instead of TPC-C. |
| `--log-level` | `info` | `trace` \| `debug` \| `info` \| `warn` \| `error`. |
| `--after-import` / `--after-test` | false | `check` mode. |
| `--help` / `-h` | — | Command help. |

### PostgreSQL (`tpcc-pgsql`)

| Flag | Default | Meaning |
| --- | --- | --- |
| `--connection` | `host=localhost dbname=tpcc user=postgres` | libpq connection string. |
| `-p` / `--path` | empty (`public`) | Schema for TPC-C tables. |
| `--partitioning` | `none` | `none` \| `warehouse_hash`. |
| `--partition-count` | `0` (derive from `-w`) | HASH modulus. |
| `--foreign_keys` | `on` | `on` \| `off`. |

### YDB (`tpcc-ydb`)

| Flag | Default | Meaning |
| --- | --- | --- |
| `--endpoint` | `localhost:2136` | `host:port` \| `grpc://` \| `grpcs://`. |
| `--database` | `/local` | YDB database (must exist). |
| `-p` / `--path` | `tpcc` | Table path prefix. |
| `--auth-scheme` | inferred | `anonymous` \| `login` \| `sa_key` \| `token`. |
| `--user` | empty | Login user. |
| `--password-env` | empty | Env var with login password. |
| `--token` | empty | Auth token (prefer `--token-env`). |
| `--token-env` | empty | Env var with token. |
| `--sa-key-file` | empty | Service-account JSON. |
| `--ca-file` | empty | PEM CA certificates. |

### OceanBase (`tpcc-oceanbase`)

| Flag | Default | Meaning |
| --- | --- | --- |
| `--connection` | `host=127.0.0.1;port=2881;user=root@test;password=tpcc;database=tpcc` | Connector string. Supports `query_timeout=<seconds>` (default 600). |
| `-p` / `--path` | connection `database` | TPC-C tables database. |
| `--partitions` | `0` | `-1` off, `0` derive from `-w`, `N` explicit (max 8192). |
| `--foreign-keys` | `on` | `on` \| `off`. |
| `--index-parallel` | `4` | `CREATE INDEX` DOP; `1` = serial. |

## TPC-C 5.11 launch-parameter checks

`mind-tpcc validate` / `test` / `aggregate.json` compare **effective**
(default-merged) settings with:

- `terminals_per_warehouse = 10`
- mix minima Payment 43% / Order-Status 4% / Delivery 4% / Stock-Level 4%
  (New-Order has no Clause 5.2.3 minimum)
- `runtime.pacing = enabled`
- `think_time_distribution = exponential`
- keying times and mean think times ≥ Clause 5.2.5.7 minima (see `workload`
  above); larger values are allowed
- `phases.measurement` ≥ 120 minutes

Deviations set `tpcc_settings_conformant: false` and populate
`tpcc_settings_deviations`. They do not reject the profile or change
`result_class` (default `engineering`).

Details: [tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md),
[specification.md](specification.md) §10.

## Result layout

```text
results/<run_id>/
├── aggregate.json
├── summary.txt
├── checks/
├── raw/loader/<instance>/
├── raw/worker/<instance>/
└── orchestrator/
    ├── profile.redacted.yaml
    ├── run-config.json
    ├── start-token.json
    ├── run-state.json
    └── orchestrator.log
```

`run-config.json` is the only declarative input distributed to loaders and
workers (concrete values, no password/token literals — only worker-local
`password_file` / `sa_key_file` / `ca_file`). Examples:
[run-config.v1.json](examples/run-config.v1.json),
[start-token.v1.json](examples/start-token.v1.json),
[aggregate.v1.json](examples/aggregate.v1.json).
