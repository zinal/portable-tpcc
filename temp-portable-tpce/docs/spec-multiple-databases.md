# Specification: Multi-Database Support (Common Libraries vs DBMS Drivers)

This document specifies the **structure** for running the TPC-E testbed from this repository against multiple DBMS types. PostgreSQL / dbt5 client-side support is implemented under `dbt5/`; future ports (YDB, MySQL, OceanBase, …) follow the same layout.

Related documents:

| Document | Role |
| --- | --- |
| [`spec-scalability.md`](spec-scalability.md) | Horizontal scaling of CE / DM / MEE / BH |
| [`spec-orchestrator.md`](spec-orchestrator.md) | `tpcectl` lifecycle (deploy / schema / load / run) |
| [`sql/pgsql/README.md`](../sql/pgsql/README.md) | PostgreSQL DDL for the client-side variant |
| [`AGENTS.md`](../AGENTS.md) | Protected TPC-E / infrastructure directories |

---

## 1. Goal

Separate **DBMS-agnostic** testbed code from **DBMS-specific** code so that:

1. Shared libraries contain Driver / Tier-A orchestration, networking, EGen integration, and transaction harness adapters that do not depend on a concrete database client.
2. Each supported DBMS has a dedicated **support library** (connection, client-side frame SQL, bulk loaders, type/dialect helpers).
3. Executables that touch the database are built **per DBMS**; executables that only speak the Driver↔Tier-A protocol remain **common**.
4. Adding a new DBMS (YDB, MySQL, OceanBase, …) means adding a new support library, schema scripts, and a small set of per-DBMS programs — not forking Driver / MEE / Validate.

Non-goals of this specification:

- Implementing YDB / MySQL / OceanBase drivers (only the extension points and layout).
- Changing TPC-E business rules, mix, or EGen data-generation semantics.
- Reintroducing or requiring server-side stored procedures.
- Modifying protected base TPC-E specification copies (see §2.2).

---

## 2. Constraints (Mandatory)

### 2.1. Client-Side Business Logic Only

All multi-DBMS variants MUST use **client-side** frame implementation (today: `BrokerageHouseMain -1` / `client_side: true` in `run-config`). Stored-procedure / server-side frame modes are **out of the portable surface**.

- Existing PostgreSQL `CDBConnectionServerSide` may remain in the PostgreSQL support library as a **legacy** path for historical / diagnostic builds.
- New DBMS ports MUST implement only the client-side frame interface.
- Orchestrator and run profiles MUST continue to assume client-side mode ([`spec-orchestrator.md`](spec-orchestrator.md) §2).

### 2.2. Protected Code (Do Not Modify)

Unless the user explicitly requests otherwise, the following remain **read-only** ([`AGENTS.md`](../AGENTS.md), [`original-files.md`](original-files.md)):

| Area | Paths |
| --- | --- |
| EGen libraries / input | `egen/egenlib/`, `egen/input/`, `egen/utils/` |
| Reference data | `data/` |
| Loader / Validate entrypoints | `dbt5/pgsql_bin/Loader/EGenLoader.cpp`, `dbt5/Validate/EGenValidate.cpp` |
| TPC root documents | `ChangeLog.*`, `EULA.*` |
| Infrastructure | `build/`, `contrib/`, `devtools/`, `library/`, `util/`, `tools/original/` |

**Implication for Loader:** per-DBMS loaders MUST reuse `EGenLoader.cpp` unchanged and supply DBMS-specific behavior only by linking a different `CCustomLoaderFactory` / table-loader implementation (today: `dbt5/pgsql/custom/`).

### 2.3. TPC-E Topology and Fair-Use Rules

Topology, connection-pool round-robin, CE partitioning, single DM, and MEE `UniqueId` / `SetBaseTime` rules from [`spec-scalability.md`](spec-scalability.md) §2 remain in force and are **independent of DBMS type**. Tier-B is “one logical database” of the chosen DBMS family.

### 2.4. Build System

Continue to use **ya make**. Libraries and programs are described with `ya.make` under project directories (`dbt5/…`). Do not introduce an alternate build system.

### 2.5. Style

Preserve the style of each directory being modified (Artistic License / dbt5 conventions under the testbed tree; TPC style under EGen). Minimize drive-by refactors.

---

## 3. Current Layout

The testbed lives under `dbt5/` with PostgreSQL as the first supported DBMS:

| Component | Path | DBMS coupling |
| --- | --- | --- |
| Abstract DB API | `dbt5/dbapi` | **None** |
| Shared runtime | `dbt5/common` | **None**: sockets / CE / DM / MEE / BH / RunConfig |
| PostgreSQL support | `dbt5/pgsql` | **PostgreSQL-only** (`libpq`, frame SQL, COPY loaders) |
| Schema | `sql/pgsql/` | **PostgreSQL-only** |
| `Driver.exe` | `dbt5/Driver` | **None** (BH endpoints only) |
| `MarketExchange.exe` | `dbt5/MarketExchange` | **None** |
| `Validate.exe` | `dbt5/Validate` | **None** (EGenValidate; no database I/O) |
| `BrokerageHouseMain-pgsql.exe` | `dbt5/pgsql_bin/BrokerageHouse` | **PostgreSQL** (`IDatabaseFactory` → client-side or legacy server-side) |
| `Loader-pgsql.exe` | `dbt5/pgsql_bin/Loader` | **PostgreSQL** when `-l CUSTOM` (via `dbt5/pgsql/custom`) |
| `TestTxn-pgsql.exe` | `dbt5/pgsql_bin/TestTransactions` | **PostgreSQL** |

PostgreSQL-specific code is confined to `dbt5/pgsql` and `dbt5/pgsql_bin`:

- `PgsqlConnection.*`, `DBConnectionClientSide.*`, `DBConnectionServerSide.*`, `pg_type_d.h`
- `PgsqlClientDatabaseFactory` / `PgsqlServerSideDatabaseFactory`
- `custom/pgloader.h` and all `PGSQL*Load.h`
- `PEERDIR(contrib/libs/libpq)` on `dbt5/pgsql` only (not on `dbt5/common`)
- `run-config` `database` object and BH CLI flags shaped for PostgreSQL (`host` / `port` / `sslmode` / `password_env`)

EGen (`egen/`) is DBMS-agnostic and MUST stay that way.

---

## 4. Architecture

```text
                         egen/  (TPC-E EGen — protected, shared)
                                   |
                                   v
              dbt5/common  — networking, CE/DM/MEE,
                             BH orchestration, Txn*DB adapters,
                             RunConfig / StartupCli (DBMS-neutral)
                                   |
                          uses abstract API
                                   v
              dbt5/dbapi  — CDBConnection interface,
                            connection params, factory hooks
                                   |
           +-----------+-----------+-----------+
           v           v           v           v
      dbt5/pgsql   dbt5/ydb    dbt5/mysql  dbt5/oceanbase
      (libpq+SQL)  (future)    (future)    (future)
      COPY loaders
           |
           +-- BrokerageHouseMain-pgsql.exe
           +-- Loader-pgsql.exe
           +-- TestTxn-pgsql.exe

   Common programs (no DBMS support library):
      Driver.exe · MarketExchange.exe · Validate.exe
```

### 4.1. Design Principles

1. **Dependency direction:** `common` depends on `dbapi` (abstract). DBMS libraries implement `dbapi` and may use `common` as needed. Common programs MUST NOT `PEERDIR` a concrete DBMS library. Per-DBMS programs link exactly one DBMS support library.
2. **One DBMS per process:** a BH / Loader / TestTxn binary is built for a single DBMS. No runtime plugin loading is required in the first phase.
3. **Factory at the edge:** construction of concrete connections / loaders happens in per-DBMS `main` (or a tiny per-DBMS glue `.cpp`), not inside shared BH worker logic via `#include` of libpq headers.
4. **Schema lives with the DBMS:** `sql/<dbms>/`, not inside C++ libraries.
5. **Names stay stable where possible:** keep class names familiar (`CDBConnection`, `CTradeOrderDB`, …) but move PostgreSQL types (`PGconn`, `PGresult`, OID helpers) out of shared headers.

---

## 5. Common Libraries

### 5.1. `dbt5/dbapi` — Abstract Database Interface

**Type:** `LIBRARY()`  
**Dependencies:** EGen headers for txn structs (`TxnHarnessStructs.h`, frame I/O types); **no** libpq / YDB / MySQL clients.

Responsibilities:

| Item | Description |
| --- | --- |
| `CDBConnection` (abstract) | Pure virtual `execute(...)` for every client-side frame used by TxnHarness; virtual `begin` / `commit` / `rollback` / `connect` / `disconnect` / `reconnect`; isolation helpers if the harness needs them |
| `TDatabaseConnectParams` | DBMS-neutral connection fields: host, port, database/name, user, password, TLS mode, plus an opaque `extra` map or connection-string override for vendor options |
| `IDatabaseFactory` | `CreateConnection(const TDatabaseConnectParams &, bool verbose)` → `CDBConnection *` |
| Error / status helpers | Neutral exception or return-code surface used by BH (no `PQresultStatus`) |

**Must leave this library:**

- `#include <libpq-fe.h>`, `PGconn *`, `PGresult *`, `Oid`, `pg_type_d.h`
- Concrete SQL strings and `PQexec*` calls
- Market-Feed / Trade-Cleanup SQL in `PgsqlConnection.cpp` (PostgreSQL library overrides of the abstract `execute` methods)

`CDBConnection` is an interface; PostgreSQL’s concrete classes (`CPgsqlConnection`, `CDBConnectionClientSide`, …) live **inside** `dbt5/pgsql` only.

### 5.2. `dbt5/common` — Testbed Runtime (DBMS-Neutral)

**Type:** `LIBRARY()`  
**Dependencies:** `egen/*`, `dbt5/dbapi`, JSON helper for RunConfig; **no** concrete DBMS client.

Located in `dbt5/common`:

| Group | Files / units |
| --- | --- |
| Networking | `CSocket`, `EndpointConnectionPool`, `BaseInterface` |
| Driver roles | `Driver`, `Customer`, `CESUT`, `DMSUT`, `MarketExchange`, `MEESUT` |
| Tier-A orchestration | `BrokerageHouse` **without** direct `new CDBConnectionClientSide` |
| Txn adapters | `TxnBaseDB`, `*DB.cpp` / `*DB.h` (BrokerVolume … TradeUpdate), `TxnHarnessSendToMarket` |
| Config / CLI | `RunConfig`, `StartupCli`, `DBT5Consts`, `CommonStructs`, locking helpers used by the above |

**BrokerageHouse change (normative):**

- `CBrokerageHouse` stores `IDatabaseFactory *` (or an equivalent creator callback) plus `TDatabaseConnectParams`.
- Worker threads call `factory->CreateConnection(params, verbose)`; the factory type is chosen in per-DBMS `main` (client-side vs legacy server-side for PostgreSQL).
- `client_side` / `-1` remain CLI / run-config concerns for legacy PostgreSQL server-side builds; `CBrokerageHouse` does not branch on them.

**Validate / Driver / MEE** continue to use only this library (plus EGen).

### 5.3. Optional Third Common Library

If `dbt5/common` becomes too large, a later split MAY extract:

- `dbt5/net` — sockets + endpoint pool; and/or
- `dbt5/config` — RunConfig + StartupCli

This is optional. The tree delivers **two** common libraries (`dbapi` + `common`) unless file ownership forces a finer cut.

---

## 6. PostgreSQL Support Library

### 6.1. `dbt5/pgsql` — Library

**Type:** `LIBRARY()`  
**Dependencies:** `dbt5/dbapi`, `dbt5/common` (`PEERDIR`), `contrib/libs/libpq`, EGen loader base classes.

Contents:

| File / unit | Role |
| --- | --- |
| `PgsqlConnection.*` | libpq connect, begin/commit, exec, escape, Market-Feed / Trade-Cleanup SQL |
| `DBConnectionClientSide.*` | Client-side frame SQL (primary portable path for PG) |
| `DBConnectionServerSide.*` | Legacy server-side frames (PG-only, not required for multi-DB) |
| `pg_type_d.h` | PG binary/text type helpers |
| `myendian.h` | PG frame SQL endian helpers |
| `custom/*` (`pgloader.h`, `PGSQL*Load.h`, `CustomLoad.*`) | `CCustomLoaderFactory` + COPY loaders |

Public surface for other PG programs:

```text
CPgsqlClientDatabaseFactory     : public IDatabaseFactory   // portable client-side path
CPgsqlServerSideDatabaseFactory : public IDatabaseFactory   // PostgreSQL legacy only
CCustomLoaderFactory            : public CBaseLoaderFactory // unchanged symbol for EGenLoader
```

### 6.2. Schema

Keep and extend under `sql/pgsql/` (already present). Future DBMS schemas:

```text
sql/
  pgsql/          # existing DDL / indexes / FKs
  ydb/            # future
  mysql/          # future
  oceanbase/      # future
```

Each directory owns dialect-specific DDL. Shared conceptual documentation (table list, partitioning rationale) may stay in `docs/` with per-DBMS notes.

### 6.3. Future DBMS Libraries (Sketch Only)

| Library | Expected contents |
| --- | --- |
| `dbt5/ydb` | YDB SDK connection, client-side frame queries / transactions, bulk upsert loaders, `CYdbDatabaseFactory` |
| `dbt5/mysql` | libmysqlclient (or equivalent), SQL frames, `LOAD DATA` / batch insert loaders |
| `dbt5/oceanbase` | MySQL-compatible or OB-specific client as chosen at implementation time; same factory + loader pattern |

Each new library MUST implement the full client-side `CDBConnection` frame set and a `CCustomLoaderFactory` (or an equivalently named factory linked into that DBMS’s `Loader`).

---

## 7. Executables

### 7.1. Classification

| Program | DBMS-specific? | Rationale |
| --- | --- | --- |
| `Driver.exe` | **No** | CE/DM → BH sockets only |
| `MarketExchange.exe` | **No** | MEE ↔ BH sockets only |
| `Validate.exe` | **No** | EGenValidate; no database I/O |
| `BrokerageHouseMain-<dbms>.exe` | **Yes** | Creates DB connections per worker; runs frame SQL |
| `Loader-<dbms>.exe` | **Yes** | `-l CUSTOM` uses DBMS bulk load API |
| `TestTxn-<dbms>.exe` | **Yes** | Direct frame execution against a live DB |

`<dbms>` ∈ { `pgsql`, `ydb`, `mysql`, `oceanbase`, … }.

### 7.2. Naming and Paths

Layout:

```text
dbt5/
  dbapi/                 # LIBRARY
  common/                # LIBRARY
  pgsql/                 # LIBRARY (PostgreSQL support)
  Driver/
    ya.make              # PROGRAM(Driver.exe)
    DriverMain.cpp
  MarketExchange/
    ya.make              # PROGRAM(MarketExchange.exe)
  Validate/
    ya.make              # PROGRAM(Validate.exe)
    EGenValidate.cpp     # protected
  pgsql_bin/
    BrokerageHouse/
      ya.make            # PROGRAM(BrokerageHouseMain-pgsql.exe)
      BrokerageHouseMain.cpp
    Loader/
      ya.make            # PROGRAM(Loader-pgsql.exe)
      EGenLoader.cpp     # protected
    TestTransactions/
      ya.make            # PROGRAM(TestTxn-pgsql.exe)
      ...
```

Notes:

- Directory name `pgsql_bin/` avoids clashing with library dir `pgsql/`. Equivalent layouts (`dbt5/bin/pgsql/…` or `dbt5/pgsql/BrokerageHouse/…` with the library sources beside them) are acceptable if `ya.make` paths stay clear.

### 7.3. Per-DBMS `main` Responsibilities

Each DBMS-specific `BrokerageHouseMain-*.cpp` / `TestTxn-*.cpp` SHOULD:

1. Parse CLI / `run-config` into `TDatabaseConnectParams` (vendor-specific field mapping allowed).
2. Instantiate the concrete `IDatabaseFactory`.
3. Construct `CBrokerageHouse` / test harness with that factory.
4. Avoid embedding SQL.

`Loader-<dbms>.exe` links `EGenLoader.cpp` + that DBMS’s loader library. `CreateLoaderFactory(CUSTOM_LOAD)` continues to return `new CCustomLoaderFactory(...)` from the linked support library — **no edits** to protected `EGenLoader.cpp`.

### 7.4. Common `main` Responsibilities

`DriverMain.cpp` / `MarketExchangeMain.cpp` stay shared. They MUST NOT gain DBMS client dependencies. Database blocks in `run-config` may be ignored by these programs (today BH-only); see §9.

---

## 8. Interface Details

### 8.1. Frame Surface

The abstract `CDBConnection` MUST expose the same `execute` overloads that TxnHarness / `CTxnBaseDB` use today for the **client-side** path, including Market-Feed and Trade-Cleanup (today partially implemented on the PostgreSQL base class).

Server-side-only entry points need not appear on the portable interface; if kept, they stay on a PG-only subclass.

### 8.2. Transactions and Isolation

Portable methods:

```text
begin() / commit() / rollback()
setReadCommitted() / setRepeatableRead() / setSerializable() / …
reconnect()
```

Each DBMS library maps these to native mechanisms (SQL `BEGIN`/`COMMIT`, YDB transaction API, etc.). If a DBMS cannot express an isolation level used by a frame, the port document for that DBMS MUST state the chosen equivalent and any TPC-E compliance impact.

### 8.3. Loader Factory Contract

Unchanged relative to EGen:

- `CBaseLoaderFactory` / per-table `CBaseLoader<ROW>` in EGen.
- Sponsor factory class name `CCustomLoaderFactory` remains the link-time symbol expected by `EGenLoader.cpp` under `COMPILE_CUSTOM_LOAD`.
- Connection string / parameters still arrive via EGen’s loader parameter (`szLoaderParms`). PostgreSQL continues to accept libpq URI/conninfo; other DBMS libraries document their own parameter syntax.

### 8.4. Forbidden Coupling

Shared headers in `dbt5/common` and `dbt5/dbapi` MUST NOT:

- include DBMS client headers;
- mention `PGconn`, `PQexec`, YDB session types, etc.;
- contain dialect-specific SQL string literals.

---

## 9. Configuration

### 9.1. `run-config` Extension

Extend the global `run-config` object ([`examples/run-config.v1.json`](examples/run-config.v1.json)) with an explicit DBMS selector:

```json
"database": {
  "dbms": "pgsql",
  "host": "localhost",
  "port": 5432,
  "name": "tpce",
  "user": "tpce",
  "sslmode": "prefer",
  "password_env": "TPCE_PGPASSWORD"
}
```

Rules:

| Field | Requirement |
| --- | --- |
| `database.dbms` | Required for new multi-DB profiles. Values: `pgsql` \| `ydb` \| `mysql` \| `oceanbase` \| … |
| Existing PG fields | Remain valid when `dbms` is `pgsql` or when `dbms` is omitted (**default `pgsql`** for backward compatibility) |
| Vendor extras | Additional keys MAY appear under `database` or `database.options`; unknown keys are ignored by programs that do not need them |

`client_side` MUST be `true` for portable runs. `client_side: false` is allowed only for legacy PostgreSQL server-side binaries.

### 9.2. Orchestrator Impact (Follow-On)

[`spec-orchestrator.md`](spec-orchestrator.md) SHOULD later:

- select artifact names (`BrokerageHouseMain-pgsql.exe`, `Loader-pgsql.exe`, …) from `database.dbms`;
- apply `sql/<dbms>/` during `schema`;
- pass DBMS-appropriate loader connection parameters.

That orchestrator work is **out of scope** for the first library-split implementation but MUST NOT be precluded by binary naming.

---

## 10. Build Layout (`ya.make`)

Normative dependency graph:

```text
egen/* --------------------+
                           +--> dbt5/dbapi
                           +--> dbt5/common --> dbt5/dbapi
contrib/libs/libpq --> dbt5/pgsql --> dbt5/dbapi (+ common as needed)

PROGRAM Driver.exe                 --> dbt5/common, egen/*
PROGRAM MarketExchange.exe         --> dbt5/common, egen/*
PROGRAM Validate.exe               --> egen/*  (dbt5/common only if still required)
PROGRAM BrokerageHouseMain-pgsql.exe --> dbt5/common, dbt5/pgsql, egen/*
PROGRAM Loader-pgsql.exe           --> dbt5/pgsql (custom loaders), egen/*
PROGRAM TestTxn-pgsql.exe          --> dbt5/common, dbt5/pgsql, egen/*
```

Top-level recurse (conceptual):

```text
RECURSE(
  dbapi
  common
  pgsql
  Driver
  MarketExchange
  Validate
  pgsql_bin
)
```

Building `./ya make dbt5` builds common programs + PostgreSQL programs. Future `./ya make dbt5/ydb_bin` adds YDB targets without rebuilding unrelated DBMS drivers.

---

## 11. Implementation Status

The layout in §3–§7 is implemented for PostgreSQL. Remaining work is limited to additional DBMS ports (§6.3) and orchestrator integration (§9.2).

---

## 12. File Ownership Summary

| Code | Library / location |
| --- | --- |
| EGen data generation, txn harness templates, input files | `egen/*`, `data/` (unchanged, protected) |
| Sockets, endpoint pools, CE/DM/MEE, BH accept loop, Txn*DB adapters, RunConfig | `dbt5/common` |
| Abstract DB connection + factory | `dbt5/dbapi` |
| libpq, PG frame SQL, PG COPY loaders, legacy server-side | `dbt5/pgsql` |
| PG DDL | `sql/pgsql/` |
| Common programs | `Driver.exe`, `MarketExchange.exe`, `Validate.exe` |
| PG programs | `BrokerageHouseMain-pgsql.exe`, `Loader-pgsql.exe`, `TestTxn-pgsql.exe` |

---

## 13. Compatibility Matrix

| Capability | PostgreSQL | Other DBMS |
| --- | --- | --- |
| Client-side frames | Required | Required for each port |
| Server-side frames | Optional legacy | Not supported |
| CUSTOM load | Required | Required for each port |
| FLAT / NULL load | Via EGen (Loader still per-DBMS binary, but flat path needs no DB client) | Same |
| Multi-instance CE/BH/MEE | Unchanged ([`spec-scalability.md`](spec-scalability.md)) | Unchanged |
| Orchestrator | Follow-on | Follow-on |

---

## 14. Testing Expectations

For PostgreSQL:

1. `./ya make dbt5` succeeds for all listed programs.
2. Existing client-side smoke (schema from `sql/pgsql`, `Loader-pgsql -l CUSTOM`, short CE/BH/MEE run) passes.
3. `Driver.exe` / `MarketExchange.exe` / `Validate.exe` link **without** `libpq`.
4. No modifications to protected EGen sources; `EGenLoader.cpp` / `EGenValidate.cpp` differ only by path from the TPC-E Tools originals.

---

## 15. Related Documentation

- [`README.md`](../README.md) — tree, build targets, binary names;
- [`docs/db-connection-sizing.md`](db-connection-sizing.md) — PostgreSQL-specific session model;
- cross-links from [`spec-scalability.md`](spec-scalability.md) / [`spec-orchestrator.md`](spec-orchestrator.md) to this document for DBMS selection.

This specification file is the normative layout reference for the testbed tree.

---

## 16. Resolved Layout Choices

| Topic | Choice in this repository |
| --- | --- |
| Per-DBMS program directory | `pgsql_bin/` (see §7.2) |
| PostgreSQL connection classes | `CPgsqlConnection` plus `CDBConnectionClientSide` / `CDBConnectionServerSide` in `dbt5/pgsql` |
| Unversioned binary aliases | Not provided; use `*-pgsql.exe` names |
| DB flag parsing | Shared `TDatabaseConnectParams` filler in `StartupCli`; per-DBMS factories in `pgsql_bin` mains |
