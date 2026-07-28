# External Dependencies for portable-tpcc

This document tracks third-party libraries required to complete the migration
from [tpcc-postgres-cpp](https://github.com/ydb-platform/tpcc-postgres-cpp) and
to implement all adapters described in [specification.md](specification.md).

## Already available in the repository

| Component | Location | Used for |
| --- | --- | --- |
| libpq | `contrib/libs/libpq` | PostgreSQL wire protocol (C API) |
| nlohmann_json | `contrib/restricted/nlohmann_json` | JSON for run-config, metrics, spec CLI |
| abseil-cpp | `contrib/restricted/abseil-cpp` | optional utilities |
| tcmalloc | `contrib/libs/tcmalloc` | optional allocator |
| util | `util/` | strings, threading, system, random |
| ya make / Go toolchain | `devtools/`, `ya` | build and future `tpccctl` |

## Missing from contrib / util / library

The following dependencies are bundled as git submodules in tpcc-postgres-cpp but
are **not** present in portable-tpcc:

| Library | Role in tpcc-postgres-cpp | Impact if absent |
| --- | --- | --- |
| **libpqxx** | PostgreSQL adapter (`PgSession`, prepared statements, transactions) | Blocks `tpcc/dbms/pgsql` adapter and all DB-integrated tests |
| **fmt** | String formatting for logs and TUI | Workaround: `util/string` (partial); full port needs fmt or equivalent |
| **spdlog** | Structured logging | Workaround: `TPCC_NO_SPDLOG` stub (current runtime build) |
| **gflags** | CLI for `tpcc-pgsql` subcommands | Blocks standalone executables until CLI is reimplemented |
| **ftxui** | Terminal UI (optional) | TUI disabled; console mode only |
| **googletest** | Unit and integration tests | Blocks automated test port from tpcc-postgres-cpp |

Additional adapters from the specification require SDKs **outside** this
repository entirely:

| SDK | Adapter |
| --- | --- |
| YDB C++ SDK | `tpcc/dbms/ydb` |
| OceanBase / MariaDB connector | `tpcc/dbms/oceanbase` |

## Current implementation status

Implemented in shared libraries (DB-agnostic, migrated from tpcc-postgres-cpp):

- `tpcc/domain` — TPC-C constants, NURand/RNG, utilities
- `tpcc/metrics` — mergeable latency histograms
- `tpcc/runtime` — coroutine scheduler, task queue, futures
- `tpcc/transactions` — abstract `ITpccSession` adapter API (skeleton)

Not yet started (blocked or pending):

- `tpcc/spec` — edition module and `tpcc-spec` CLI
- `tpcc/generator` — deterministic load and transaction inputs
- `tpcc/transactions` — shared business-operation implementations
- `tpcc/runtime` — terminal state machines, phase control, retry loop
- `tpcc/loader`, `tpcc/checks` — load plan and invariant catalog
- `tpcc/dbms/pgsql` — **blocked on libpqxx** (or a libpq-only rewrite)
- `tpcc/app/pgsql`, `tools/tpccctl` — blocked on adapter + gflags/Go respectively

## Recommended next steps

Choose one path before continuing the PostgreSQL adapter:

### Option A — Add libpqxx to contrib (recommended for fastest port)

1. Vendor libpqxx (and its libpq dependency is already satisfied) into
   `contrib/restricted/libpqxx` via yamaker or manual import.
2. Port `PgSession`, `PgConnectionPool`, and SQL from tpcc-postgres-cpp into
   `tpcc/dbms/pgsql`.
3. Keep shared transaction logic in `tpcc/transactions`.

**Pros:** Minimal rewrite; tpcc-postgres-cpp code maps almost 1:1.
**Cons:** Requires contrib maintenance and license review.

### Option B — Implement PostgreSQL adapter on libpq directly

1. Use existing `contrib/libs/libpq` with a thin async/wrapper layer in
   `tpcc/dbms/pgsql`.
2. Reimplement prepared-statement and result parsing currently done by libpqxx.

**Pros:** No new contrib packages.
**Cons:** Larger engineering effort; must re-validate all transaction SQL paths.

### Option C — Staged delivery: shared logic first, defer DB adapters

1. Complete `domain`, `generator`, `runtime` (terminals), and fake-adapter tests.
2. Add libpqxx / SDKs in a follow-up change set.

**Pros:** Unblocks spec module, orchestrator contract, and cross-DB equivalence design.
**Cons:** No end-to-end PostgreSQL smoke test until adapters land.

### Supporting libraries (fmt, spdlog, gflags, gtest)

| Approach | Notes |
| --- | --- |
| Vendor into contrib | Matches tpcc-postgres-cpp layout; familiar to porters |
| Replace with util + minimal CLI | Reduces contrib surface; more rewrite |
| Hybrid | gtest + libpqxx in contrib; logging via util; CLI via lightweight parser |

## Decision requested

Implementation is **paused** at the PostgreSQL adapter boundary until one of
options A–C is chosen and, for A or the hybrid path, the missing contrib
packages are approved for import.
