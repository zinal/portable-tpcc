# External Dependencies for portable-tpcc

This document tracks third-party libraries required to complete the migration
from [tpcc-postgres-cpp](https://github.com/ydb-platform/tpcc-postgres-cpp) and
to implement all adapters described in [specification.md](specification.md).

## Available in the repository

| Component | Location | Used for |
| --- | --- | --- |
| libpq | `contrib/libs/libpq` | PostgreSQL wire protocol (C API) |
| libpqxx | `contrib/libs/libpqxx` (7.2.0) | PostgreSQL adapter (`PgSession`, COPY, transactions) |
| fmt | `contrib/libs/fmt` | String formatting for logs and TUI |
| spdlog | `contrib/restricted/spdlog` | Structured logging |
| gflags | `contrib/libs/gflags` | CLI for `tpcc-pgsql` |
| ftxui | `contrib/libs/ftxui` | Terminal UI (optional, enabled in `tpcc-pgsql`) |
| googletest | `contrib/restricted/googletest` | Unit and integration tests (not yet ported) |
| nlohmann_json | `contrib/restricted/nlohmann_json` | JSON for run-config, metrics, spec CLI |
| abseil-cpp | `contrib/restricted/abseil-cpp` | optional utilities |
| tcmalloc | `contrib/libs/tcmalloc` | optional allocator |
| util | `util/` | strings, threading, system, random |
| ya make / Go toolchain | `devtools/`, `ya` | build and future `tpccctl` |

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
| `tpcc/domain` | done | constants, NURand/RNG, utilities |
| `tpcc/metrics` | done | mergeable latency histograms |
| `tpcc/runtime` | done | coroutine scheduler, futures, logging, thread pool |
| `tpcc/transactions` | skeleton | abstract `ITpccSession` API (per specification) |

### PostgreSQL adapter and executable

| Module | Status | Notes |
| --- | --- | --- |
| `tpcc/dbms/pgsql` | done (initial port) | session, pool, transactions, init/import/run/check |
| `tpcc/app/pgsql` (`tpcc-pgsql`) | done (initial port) | CLI: init, import, run, clean, check |

### Not yet started

| Module | Blocker |
| --- | --- |
| `tpcc/spec` | edition module and `tpcc-spec` CLI |
| `tpcc/generator` | deterministic load and transaction inputs (spec) |
| `tpcc/loader`, `tpcc/checks` | horizontal scaling (spec) |
| `tpcc/runtime` (terminal phases) | phase barriers, retry loop per spec |
| `tpcc/dbms/ydb`, `tpcc/dbms/oceanbase` | external SDKs |
| `tools/tpccctl` | initial | Go orchestrator (`validate`, `plan`, `deploy`, `run`, etc.) |
| unit/integration tests | port from `tpcc-postgres-cpp/src/ut` |

## SDKs outside this repository

| SDK | Adapter |
| --- | --- |
| YDB C++ SDK | `tpcc/dbms/ydb` |
| OceanBase / MariaDB connector | `tpcc/dbms/oceanbase` |

These require separate vendoring or environment setup; implementation is paused
until an explicit decision on SDK packaging is made.
