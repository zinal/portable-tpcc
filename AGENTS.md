# Instructions for AI agents

This document defines mandatory rules for automated agents (Cursor, Cloud Agents, and similar tools) working with the **portable-tpcc** repository.

**Protocol and requirements** (remote process contract, check reports, role
behavior, configuration model) are defined in
[`docs/specification.md`](docs/specification.md). Adapter surfaces are in
[`docs/adapter-api.md`](docs/adapter-api.md). Per-DBMS settings are in
[`docs/run-ydb.md`](docs/run-ydb.md),
[`docs/run-pgsql.md`](docs/run-pgsql.md),
[`docs/run-oceanbase.md`](docs/run-oceanbase.md), and
[`docs/parameter-reference.md`](docs/parameter-reference.md).

When a task involves orchestration, remote launches, artifacts, integrity
checks, or profile/run-config semantics, **read the specification first**. Do
not reconstruct those rules from source. If code and spec disagree, treat the
spec as the requirement, implement or flag the gap, and do not silently
“fix” the protocol in only one DBMS copy.

## 1. Preserve the coding style

Before making changes, study the surrounding code and **preserve the style used in the repository**:

- **Naming and structural conventions** — follow the patterns in neighboring files (`C` class prefixes, `TIdent`/`INT32` types, TPC/Artistic License header style, and so on).
- **Formatting** — do not reformat code incidentally; the diff must contain only the changes required for the task.
- **Dependencies and builds** — use the existing build system for each component;
  do not add alternative build systems unless explicitly requested. The
  `mind/` module is built with the standard Go toolchain, not `ya make`.

If a task affects multiple subsystems, follow the style of **each specific directory being modified**, not a “universal” modern C++ style.

## 2. Analyze carefully before making changes

**Do not make changes until you have completed this analysis:**

1. **Understand the task** — determine which directories and components are
   affected and whether the proposed solution fits the repository
   architecture. For protocol, orchestrator/binary interaction, check reports,
   or configuration requirements, read
   [`docs/specification.md`](docs/specification.md) (especially §9.1–§9.2 for
   remote processes and integrity checks).
2. **Study the context** — read the files to be modified and their immediate consumers/dependencies; inspect `ya.make` for ya modules or `go.mod` for `mind/`.
3. **Assess the scope** — minimize the diff; do not refactor or “improve” code outside the task’s scope.
4. **Check the restrictions** — make sure the plan does not violate the prohibitions in sections 4 and 5 below.
5. **Verify the changes** — whenever possible, build the affected targets and
   run the relevant tests using the component's build system.

If the task is ambiguous or requires changes in protected areas, **stop and ask the user for explicit confirmation** instead of assuming permission.

## 3. Building with ya make

The build system is invoked via the **`./ya` launcher script in the repository root** (not a system-wide `ya` binary). Run all `ya make` commands from the repo root, for example:

```bash
./ya make tpcc/dbms/pgsql
./ya make -t tpcc/runtime/ut
```

On first use the script may download the ya binary; subsequent builds reuse it. Do not assume `ya` is on `PATH` — always use `./ya`.

### Building the YDB variant

The YDB app (`tpcc/app/ydb`, binary `tpcc-ydb`) **must** be built with CUDA
disabled. Use these defines (release build shown; drop `-r` for the ya
default/debug configuration):

```bash
./ya make -r -DHAVE_CUDA=no -DCUDA_VERSION=11.4 tpcc/app/ydb
```

When building the full `tpcc/` tree (which includes YDB), pass the same
`-DHAVE_CUDA=no -DCUDA_VERSION=11.4` flags. The root convenience script
`./build.sh` always adds them for C++ targets.

Do not omit these flags for YDB builds: the ya make graph otherwise expects
a CUDA toolchain that is not part of the portable-tpcc development setup.

### Building mind-tpcc with Go

`mind/` is a standalone Go module and **must not** be added to the `ya make`
graph. Build and test it from the repository root with the standard Go
toolchain:

```bash
go -C mind build ./cmd/mind-tpcc
go -C mind test ./...
```

When changing dependencies, use Go module commands such as
`go -C mind get` and `go -C mind mod tidy`.

## 4. Do not modify infrastructure directories

**Do not modify or add files** in the following directories:

| Directory | Purpose |
| --- | --- |
| [`build/`](build/) | ya make build system configuration |
| [`contrib/`](contrib/) | third-party dependencies |
| [`devtools/`](devtools/) | ya utility source code |
| [`library/`](library/) | core libraries of the build ecosystem |
| [`util/`](util/) | shared utilities of the ya make ecosystem (the directory is named `util/`, not `utils/`, in the ya make tree) |

**Exception:** the user **explicitly** asks to modify files in one of these directories (by naming the directory or specific paths). General requests such as “fix the build” that do not name an infrastructure directory **do not count** as explicit permission.

Files in these directories may be **read** to understand the build and dependencies.

## 5. Recommended workflow

1. Read this document, [`README.md`](README.md), and
   [`docs/specification.md`](docs/specification.md) for protocol and
   requirements (do not infer them from source when the spec defines them).
2. Determine which directories are affected and check the restrictions (sections 4–5).
3. Study the style and dependencies of the target files.
4. Plan a minimal diff and obtain the user’s approval if exceptions to the prohibitions are required.
5. Make the changes, then build and test the affected components.
6. Explain in the commit/PR which restrictions were taken into account and why the changes are safe for TPC-C compatibility.

## 6. Specification map

Protocol and requirements live in the documents below. **Follow those
documents.** Do not copy protocol rules into this file, and do not reconstruct
them from source when the spec defines them. If code and spec disagree, treat
the spec as the requirement (see the introduction).

| Topic | Where |
| --- | --- |
| Remote `process.json` / nonce / `artifact-manifest.json`, launch-time `--threads` | [specification.md](docs/specification.md) §9.1 |
| Integrity checks (JSON reports, query timeout, check `--threads`, session concurrency, warehouse-range size, catalog scheduling, stdout vs JSON) | [specification.md](docs/specification.md) §9.2 |
| Adapter check/session API | [adapter-api.md](docs/adapter-api.md) §3.6, §4.4, §7 |
| Adapter async `ITpccTransaction` (no scheduler `.Get()` / `GetValueSync`) | [adapter-api.md](docs/adapter-api.md) §4.3, [specification.md](docs/specification.md) §4.2 / §7 |
| OceanBase `query_timeout` / physical options | [specification.md](docs/specification.md) §11, [run-oceanbase.md](docs/run-oceanbase.md) |
| Profile and CLI parameters | [parameter-reference.md](docs/parameter-reference.md) |

Check role **implementation** (not the protocol source of truth):
`tpcc/checks/`, `tpcc/dbms/{pgsql,ydb,oceanbase}/check.cpp`,
`mind/internal/config/plan.go`, `mind/internal/orchestrator/`.
