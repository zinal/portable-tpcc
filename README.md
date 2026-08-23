# portable-tpcc

A horizontally scalable TPC-C implementation with shared workload logic,
YDB/PostgreSQL/OceanBase adapters, and a dedicated orchestrator.

Results MUST NOT be called official TPC-C results without the required TPC
verification.

## Running a test

Two modes:

1. **Standalone** — drive `tpcc-<dbms>` directly (simplest for local smoke tests).
2. **Orchestrated** — use `mind-tpcc` with a YAML profile (multi-host and full
   pipeline).

Per-DBMS instructions (DB-specific settings plus the minimum global profile
fields needed to run):

- [PostgreSQL](docs/run-pgsql.md) (`tpcc-pgsql`)
- [YDB](docs/run-ydb.md) (`tpcc-ydb`)
- [OceanBase](docs/run-oceanbase.md) (`tpcc-oceanbase`)

Complete parameter reference (profile YAML, CLI flags, environment variables):
[docs/parameter-reference.md](docs/parameter-reference.md).
`mind-tpcc configure --profile <path> --dbms <pgsql|ydb|oceanbase>` writes a
complete starter profile with every field set to the built-in default.

Build everything from the repository root with `./build.sh` (it always passes
`-DHAVE_CUDA=no -DCUDA_VERSION=11.4` for C++ targets; required by `tpcc-ydb`).
Individual `./ya make` / `go -C mind build` commands are in the per-DBMS
guides.

Do not put passwords in profile YAML/JSON. Pass them via the standalone
connection string or an environment variable named in `password_env` on the
control host. `mind-tpcc` copies that value to a mode-0600 `db-password` file
on each worker and points `run-config.json` at `password_file`.

For settings closer to TPC-C 5.11, see the defaults embedded in `mind-tpcc` and
[docs/tpcc-5.11-conformance-analysis.md](docs/tpcc-5.11-conformance-analysis.md)
(for example measurement interval ≥ 120 minutes).

## Architecture

- [specification](docs/specification.md);
- [shared libraries and adapter API](docs/adapter-api.md);
- [async `ITpccTransaction` migration](docs/async-adapter-transactions.md);
- [profile example (YDB)](docs/examples/profile.ydb.v1.yaml);
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
