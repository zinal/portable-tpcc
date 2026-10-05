# Running TPC-C against the dummy adapter

Binary: `tpcc-dummy`. Full parameter list:
[parameter-reference.md](parameter-reference.md).

The dummy adapter does not open a real database. Schema, load, indexes, drop,
and integrity checks are empty and succeed. The **test** (worker) path uses the
shared terminal, phase controller, TPC-C workflows, retry, and metrics stack.
Each adapter round-trip (`Execute`, `ExecuteBatch`, `Commit`,
`Rollback`, `Cancel`, `ExecuteSelect1`, `ExecuteFinalAndCommit`) waits a
uniformly random duration in `[delay_us_min, delay_us_max]` microseconds so
you can exercise orchestration and histograms without a DBMS.

Two modes:

1. **Standalone** — drive `tpcc-dummy` directly.
2. **Orchestrated** — use `mind-tpcc` with `database.dbms: dummy`.

Results MUST NOT be called official TPC-C results.

## Build

```bash
./ya make tpcc/app/dummy
go -C mind build ./cmd/mind-tpcc
```

Binaries:

| Binary | Path after build |
| --- | --- |
| `tpcc-dummy` | `tpcc/app/dummy/tpcc-dummy` |
| `mind-tpcc` | `mind/mind-tpcc` |

For orchestration, copy `tpcc-dummy` into the profile's `paths.local_artifacts`
directory (default `.`). Re-run `mind-tpcc deploy` after rebuilding.

## Dummy-specific settings

| Standalone | Profile | Meaning |
| --- | --- | --- |
| `--connection` | `database.options` | `delay_us_min=N delay_us_max=M` vs YAML keys below. |
| `--delay-us-min` | `database.options.delay_us_min` | Inclusive minimum simulated latency in **microseconds** (default **0**). Used when `--connection` is empty. |
| `--delay-us-max` | `database.options.delay_us_max` | Inclusive maximum (default **0**). Must be ≥ min. If only min is set in the connection string or run-config, max copies min. |
| — | `database.dbms: dummy` | Required in the profile. |
| — | `database.endpoint` | Required by the profile schema; dummy does not connect. Default `localhost`. |

No password, user, or auth fields are required. Unknown `database.options.*`
keys are rejected for `dbms=dummy`.

Each sampled delay is independent of the TPC-C workload RNG. `0`/`0` completes
adapter futures immediately (still on the shared async session API).

## Standalone

```bash
BIN=./tpcc/app/dummy/tpcc-dummy
$BIN schema
$BIN import -w 10
$BIN indexes
$BIN run -w 10 --duration=1 --no-delays --delay-us-min=100 --delay-us-max=500
$BIN check -w 10 --after-test
$BIN debug -w 10
```

`--connection="delay_us_min=100 delay_us_max=500"` is equivalent to the two
delay flags when `--connection` is set (it wins over the flags).

## Orchestrated

```text
mind-tpcc configure --profile ./profile.yaml --dbms dummy \
  --delay-us-min 100 --delay-us-max 500 \
  --insecure-ignore-host-key
```

`mind-tpcc run` then drives schema → load → indexes → test → collect using
`tpcc-dummy` on the loader/worker hosts. Checks (`--after-import` /
`--after-test`) report every catalog id as passed.
