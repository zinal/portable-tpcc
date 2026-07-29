# External Dependencies for portable-tpcc

This document tracks third-party libraries required to complete the migration
from [tpcc-postgres-cpp](https://github.com/ydb-platform/tpcc-postgres-cpp) and
to implement all adapters described in [specification.md](specification.md)
and [adapter-api.md](adapter-api.md).

Alignment sequencing: [alignment-plan.md](alignment-plan.md).
Gap analysis: [implementation-gap-analysis.md](implementation-gap-analysis.md).

## Available in the repository

| Component | Location | Used for |
| --- | --- | --- |
| libpq | `contrib/libs/libpq` | PostgreSQL wire protocol (C API) |
| libpqxx | `contrib/libs/libpqxx` (7.2.0) | PostgreSQL adapter (`PgSession`, COPY, transactions) |
| fmt | `contrib/libs/fmt` | String formatting for logs and TUI |
| spdlog | `contrib/restricted/spdlog` | Structured logging |
| gflags | `contrib/libs/gflags` | CLI for `tpcc-pgsql` |
| ftxui | `contrib/libs/ftxui` | Terminal UI (optional, enabled in `tpcc-pgsql`) |
| googletest | `contrib/restricted/googletest` | Unit and integration tests |
| nlohmann_json | `contrib/restricted/nlohmann_json` | JSON for run-config and metrics |
| abseil-cpp | `contrib/restricted/abseil-cpp` | optional utilities |
| tcmalloc | `contrib/libs/tcmalloc` | optional allocator |
| util | `util/` | strings, threading, system, random |
| ya make / Go toolchain | `devtools/`, `ya`; Go-native tools for `tpccctl` | C++ build; Go modules for orchestrator |

## libpqxx version note

`tpcc-postgres-cpp` targets a newer libpqxx API (`pqxx::params` in the public
namespace, `stream_to::table`, `result::one_row`, `connection::quote_table`).
The vendored **libpqxx 7.2.0** uses a slightly different API. The PostgreSQL
adapter bridges this gap via:

- `tpcc/dbms/pgsql/pqxx_compat.h` — helpers for COPY streams;
- variadic `PgSession::ExecuteQuery` / `ExecuteModify` built on `exec_params`;
- direct `exec_params` calls where the original code used `params.append()`.

Upgrading libpqxx in `contrib/` to 7.9+ would reduce the shim layer but is not
required for the current port.

## Implementation status

### Shared libraries (DB-agnostic)

| Module | Status | Source |
| --- | --- | --- |
| `tpcc/domain` | in progress | constants, `TMoney`/`TRate`, seeded + legacy RNG |
| `tpcc/metrics` | done | mergeable latency histograms (serialization still missing) |
| `tpcc/runtime` | done | coroutine scheduler, futures, logging, thread pool |
| `tpcc/transactions` | in progress | async `ITpccSession` + `TSemanticOp` variant (`ops.h`) |
| `tpcc/generator` | in progress | deterministic population; wired into PG `ImportSync` |
| `tpcc/loader` | skeleton | `ILoadAdapter` / `PutBatch` header only |
| `tpcc/checks` | not started | still inside `dbms/pgsql/check.*` |
### PostgreSQL adapter and executable

| Module | Status | Notes |
| --- | --- | --- |
| `tpcc/dbms/pgsql` | transitional | session, pool, SQL workflows, init/import/run/check |
| `tpcc/app/pgsql` (`tpcc-pgsql`) | transitional | CLI aliases + `worker`/`loader`; not yet on abstract session API |

### Not yet started / blocked

| Module | Notes |
| --- | --- |
| Idempotent PG `PutBatch` | generator exists; import still uses non-idempotent COPY |
| Shared checks package | lift from pgsql |
| Runtime `--start-at` phases | wall-clock sync per specification §7 |
| `tpcc/dbms/ydb`, `oceanbase` | external SDKs |
| `tools/tpccctl` remote drive | plan/assignment done; SSH/start-at incomplete |

## Known defects (tracked)

These are intentional interim gaps while Phases 1–5 of the alignment plan land:

1. **Load not idempotent** — plain `COPY`; retries can PK-conflict.
2. **Money path still uses `double` in PG transaction workflows** — `TMoney`
   is used for load; New-Order/Payment/`query_result` still convert via
   `double`.
3. **Workers emit percentiles** — not raw histogram buckets (spec §8).
4. **No `--start-at` handling** — workers start on local clocks.
5. **SQLSTATE error classifier missing** — only `transaction_rollback` retries.
6. **Run-config `workload.*` largely ignored** — mix/timings hardcoded.

## SDKs outside this repository

| SDK | Adapter |
| --- | --- |
| YDB C++ SDK | `tpcc/dbms/ydb` |
| OceanBase / MariaDB connector | `tpcc/dbms/oceanbase` |

These require separate vendoring or environment setup; implementation is paused
until an explicit decision on SDK packaging is made.
