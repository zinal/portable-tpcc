# External Dependencies for portable-tpcc

This document tracks third-party libraries required to complete the migration
from [tpcc-postgres-cpp](https://github.com/ydb-platform/tpcc-postgres-cpp) and
to implement all adapters described in [specification.md](specification.md)
and [adapter-api.md](adapter-api.md).

Alignment sequencing: [alignment-plan.md](alignment-plan.md).
Worker `ITpccTransaction` async migration:
[async-adapter-transactions.md](async-adapter-transactions.md).
TPC-C 5.11 conformance notes:
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md).

## Available in the repository

| Component | Location | Used for |
| --- | --- | --- |
| libpq | `contrib/libs/libpq` | PostgreSQL wire protocol (C API) |
| libpqxx | `contrib/libs/libpqxx` (7.2.0) | PostgreSQL adapter (`PgSession`, COPY, transactions) |
| fmt | `contrib/libs/fmt` | String formatting for logs and console output |
| library/cpp/logger | `library/cpp/logger` | Structured logging (shared with YDB SDK) |
| gflags | `contrib/libs/gflags` | CLI for `tpcc-pgsql` |
| ftxui | `contrib/libs/ftxui` | Removed / no longer used by tpcc (vendored copy retained) |
| googletest | `contrib/restricted/googletest` | Unit and integration tests |
| nlohmann_json | `contrib/restricted/nlohmann_json` | JSON for run-config and metrics |
| OceanBase Connector/C | `contrib/restricted/obconnector-c` | OceanBase adapter (`TObSession`, prepared statements, multi-row insert loader) |
| abseil-cpp | `contrib/restricted/abseil-cpp` | optional utilities |
| tcmalloc | `contrib/libs/tcmalloc` | optional allocator |
| util | `util/` | strings, threading, system, random |
| ya make / Go toolchain | `devtools/`, `ya`; Go-native tools for `mind-tpcc` | C++ build; Go modules for orchestrator |

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
| `tpcc/domain` | done | constants, `TMoney`/`TRate`, seeded + legacy RNG |
| `tpcc/metrics` | done | mergeable latency histograms; workers emit raw buckets |
| `tpcc/runtime` | done | coroutine scheduler, futures, logging, thread pool |
| `tpcc/transactions` | done | async `ITpccSession`, `TSemanticOp` (`ops.h`), five workflows |
| `tpcc/generator` | done | deterministic population; wired into PG `ImportSync` |
| `tpcc/loader` | in progress | `ILoadAdapter`; PG PutBatch regenerates rows from seed |
| `tpcc/checks` | done | catalog, `ICheckAdapter`, JSON report writer |

### PostgreSQL adapter and executable

| Module | Status | Notes |
| --- | --- | --- |
| `tpcc/dbms/pgsql` | transitional | session, pool, load/admin/check adapters, terminal runtime |
| `tpcc/app/pgsql` (`tpcc-pgsql`) | transitional | normative roles + legacy aliases; orchestrated `check`/`schema` |
| `tpcc/dbms/ydb` | in progress | YDB query/table SDK adapter; shared workflows via `ITpccSession`; BulkUpsert loader |
| `tpcc/app/ydb` (`tpcc-ydb`) | in progress | normative roles + legacy aliases for YDB |
| `tpcc/dbms/oceanbase` | done | Connector/C transport, admin/load/session/check adapters, terminal runtime |
| `tpcc/app/oceanbase` (`tpcc-oceanbase`) | done | normative roles + legacy aliases for OceanBase |
| `mind-tpcc` | done (Phase 5) | SSH/local remote drive, `--start-at`, collect/consolidate |

## Remaining work (tracked)

Architecture / product (see [alignment-plan.md](alignment-plan.md) Phase 6):

1. **PutBatch row payloads** — PG adapter regenerates from seed; shared-loader
   serialized rows are not yet consumed.
2. **Worker `ITpccTransaction` still blocks the scheduler** — PG/OB `.Get()`,
   YDB `GetValueSync()`, so paced `Inflight ≈ ThreadCount`. Target contract:
   [adapter-api.md](adapter-api.md) §4.3.0; sequence:
   [async-adapter-transactions.md](async-adapter-transactions.md).
3. Open decisions from specification §14 (ambiguous-commit policy, canonical
   row bytes, minimum PG version). Histogram layout resolved as `linear_exp`
   (`unit` + `highest`).
4. Broader unit/integration test coverage.
5. OceanBase integration validation against real multi-node deployments.

TPC-C 5.11 engineering deviations and open defects:
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md).

## SDKs outside this repository

| SDK | Adapter |
| --- | --- |
| YDB C++ SDK | `tpcc/dbms/ydb` |
| OceanBase Connector/C | `tpcc/dbms/oceanbase` |

OceanBase Connector/C is vendored at `contrib/restricted/obconnector-c` and is
used by `tpcc/dbms/oceanbase`. The YDB C++ SDK is already available in this
repository and is used by `tpcc/dbms/ydb`.
