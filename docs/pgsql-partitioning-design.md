# PostgreSQL TPC-C table partitioning design

This document proposes physical partitioning for the PostgreSQL adapter
(`tpcc/dbms/pgsql`) to improve large-scale TPC-C performance while keeping
partition pruning aligned with the main workload queries and enabling useful
local indexes.

Status: **design proposal** (not yet implemented).
Related contract: [adapter-api.md](adapter-api.md) §5 (adapters MAY add
partitions), [specification.md](specification.md) §11.

## 1. Goals

1. Reduce bloat and contention on large tables (`stock`, `customer`,
   `order_line`, `oorder`, `new_order`, `history`) at high warehouse counts.
2. Ensure every main-scenario query that touches a partitioned table supplies
   an equality predicate on the partition key so PostgreSQL can prune to one
   (or a small set of) partitions — no full partition fan-out in the hot path.
3. Keep primary keys and secondary indexes meaningful **inside** each
   partition (local B-trees stay compact and selective).
4. Preserve logical TPC-C semantics, loader idempotency
   (`DELETE warehouse … CASCADE` + `COPY`), and optional foreign keys.
5. Record the physical choice in capabilities / run settings
   (`PartitioningStyle`), consistent with YDB’s `warehouse_range`.

Non-goals for the first iteration:

- Changing shared transaction logic or semantic ops.
- Auto-attaching partitions when scale grows after schema creation.
- Subpartitioning by district (see §7).

## 2. Workload access patterns (why warehouse RANGE)

All five TPC-C transactions bind SQL in
`tpcc/dbms/pgsql/tpcc_session.cpp`. Warehouse-scoped tables are always
filtered by a warehouse id column (and usually district):

| Transaction | Dominant keys | Cross-warehouse |
| --- | --- | --- |
| New-Order | home `w_id` / `d_id`; stock by `s_w_id,s_i_id`; item by `i_id` | ~1% lines: remote `s_w_id` point RMW |
| Payment | home WH/district YTD; customer by `c_w_id,c_d_id,(c_id\|c_last)` | 15% remote customer; `history` stores both `h_w_id` and `h_c_w_id` |
| Order-Status | home customer + latest `oorder` + `order_line` by order | none |
| Delivery | home WH, loop districts 1–10; oldest `new_order` per district | none |
| Stock-Level | home district; `order_line` range + join home `stock` | none |

Queries **without** a warehouse predicate: only `item` by `i_id` (global
catalog, 100 000 rows). Everything else is point or short-range access with
an equality filter on `*_w_id` / `w_id`.

Therefore the natural partition key for warehouse-local tables is the leading
warehouse column already present in every local primary key — the same axis
YDB uses (`PartitioningStyle = "warehouse_range"`).

## 3. Recommended scheme

### 3.1. Style

| Setting | Value |
| --- | --- |
| Method | Declarative **`PARTITION BY RANGE (warehouse_id)`** |
| Capability string | `warehouse_range` (align with YDB) |
| Bounds | Fixed at schema time from `scale.warehouses` / `--warehouses` |
| Granularity | Multiple warehouses per partition (not 1:1) |
| `item` | **Not partitioned** (global, point lookup by `i_id`) |
| `warehouse`, `district` | **Not partitioned** in v1 (tiny; simplify FK roots) |

Hash partitioning was considered: it also prunes on equality and does not need
bounds up front, but RANGE matches the YDB splitter mental model, keeps
loader warehouse ranges contiguous on disk, and makes “warehouses per
partition” an explicit tunable.

### 3.2. Tables to partition

| Table | Partition key | Notes |
| --- | --- | --- |
| `stock` | `s_w_id` | Largest hot table; New-Order + Stock-Level |
| `customer` | `c_w_id` | Payment / Order-Status name+id; Delivery update |
| `oorder` | `o_w_id` | Order-Status latest-order; Delivery |
| `new_order` | `no_w_id` | Delivery `ORDER BY no_o_id LIMIT 1` |
| `order_line` | `ol_w_id` | Order-Status / Delivery / Stock-Level (home WH) |
| `history` | `h_w_id` | Insert-only in measurement; key = payment warehouse |

`ol_supply_w_id` is **not** the partition key for `order_line`: lines are always
written and read under the **home** order warehouse (`ol_w_id`). Remote stock
is accessed via `stock.s_w_id`, which is partitioned correctly.

### 3.3. Partition sizing

Reuse the YDB heuristics from `tpcc/dbms/ydb/data_splitter.cpp` (or extract a
tiny shared helper under `tpcc/domain/` / adapter-local copy to avoid coupling):

- Target ~2 GiB per partition using approximate MB/warehouse:
  - `stock` ≈ 45 MB/WH
  - `order_line` ≈ 35 MB/WH (grows during run; initial sizing is a lower bound)
  - `customer` ≈ 20 MB/WH
  - `history` ≈ 2.4 MB/WH initial (grows)
  - `oorder` ≈ 1.5 MB/WH
- Floor: `max(50, warehouses / 100)` partitions for large tables when the MB
  heuristic would create fewer.
- Emit bounds `FOR VALUES FROM (lo) TO (hi)` covering `[1, warehouses+1)`.
- Always create a final catch-all partition
  `FOR VALUES FROM (warehouses+1) TO (MAXVALUE)` (and optionally
  `FROM (MINVALUE) TO (1)`) so misconfiguration fails loudly on insert rather
  than aborting with “no partition found” only after partial load — or omit
  catch-all and fail fast; prefer **fail fast** for TPC-C fixed scale.

Example for `W = 1000`, ~45 MB/WH stock → ~45 WH/partition → ~23 partitions.

### 3.4. DDL sketch

```sql
CREATE TABLE stock (
    s_w_id int NOT NULL,
    s_i_id int NOT NULL,
    -- … columns …
    PRIMARY KEY (s_w_id, s_i_id)
) PARTITION BY RANGE (s_w_id);

CREATE TABLE stock_p001 PARTITION OF stock
    FOR VALUES FROM (1) TO (46);
-- … more partitions …
```

Primary keys already contain the partition column, satisfying PostgreSQL’s
rule that unique constraints on partitioned tables include the partition key.
The existing unique constraint `idx_order (o_w_id, o_d_id, o_c_id, o_id)` is
likewise valid.

`InitSync` must become warehouse-count-aware (YDB already is):

```text
TPgAdminAdapter(connection, path, warehouseCount)
  → InitSync(connection, path, warehouseCount)
```

CLI `tpcc schema` and the orchestrated schema role must pass
`scale.warehouses` / `--warehouses` (today schema ignores warehouse count).

## 4. Indexes after partitioning

Partitioning does not replace indexes; it makes them local and smaller.

| Index | Definition | Role after partitioning |
| --- | --- | --- |
| PK per table | unchanged, warehouse-leading | Local unique index per partition; point lookups prune then seek |
| `idx_order` | `UNIQUE (o_w_id, o_d_id, o_c_id, o_id)` | Order-Status “latest order for customer”; still local |
| `idx_customer_name` | `(c_w_id, c_d_id, c_last, c_first)` | Payment / Order-Status by last name; prune on `c_w_id` then local scan |

Optional follow-ups (not required for correctness):

1. **Delivery assist** — if `EXPLAIN` shows a suboptimal plan for
   `WHERE no_w_id=$1 AND no_d_id=$2 ORDER BY no_o_id LIMIT 1 FOR UPDATE`,
   the PK `(no_w_id, no_d_id, no_o_id)` already matches; partitioning mainly
   shrinks the leaf pages scanned. No extra index expected.
2. **Stock-Level** — range on `order_line (ol_w_id, ol_d_id, ol_o_id)` is the
   PK prefix; join to `stock` is point by `(s_w_id, s_i_id)`. Partition pruning
   on both sides is the main win; avoid a global GIN/hash on `ol_i_id`.
3. Do **not** drop leading `*_w_id` from secondary index definitions on the
   parent table: PostgreSQL attaches the same key to each partition, and the
   leading column documents the access path used by shared SQL. Compact
   per-partition keys like `(c_d_id, c_last, c_first)` are a later micro-opt
   only if measured.

Index creation timing stays as today: defer `idx_customer_name` until after
`COPY` (`CreateIndexes`); keep `idx_order` with table DDL (or move both
post-load — either is fine if load path is updated consistently).

## 5. Foreign keys

Current DDL uses `ON DELETE CASCADE` so idempotent warehouse reload can
`DELETE FROM warehouse WHERE w_id = $1` and wipe dependents. That behavior
should remain.

PostgreSQL allows foreign keys **to** and **between** partitioned tables when
referenced unique keys include the partition columns. Proposed FK matrix for
v1:

| From | To | OK with warehouse RANGE? |
| --- | --- | --- |
| `stock(s_w_id)` → `warehouse(w_id)` | unpartitioned parent | yes |
| `stock(s_i_id)` → `item(i_id)` | unpartitioned | yes |
| `district` → `warehouse` | both unpartitioned | yes |
| `customer` → `district` | partitioned → unpartitioned | yes |
| `oorder` → `customer` | both partitioned on same WH key | yes (keys align) |
| `new_order` → `oorder` | same | yes |
| `order_line` → `oorder` | same home WH | yes |
| `order_line(ol_supply_w_id, ol_i_id)` → `stock` | supply WH may ≠ home WH | yes: FK columns are exactly `stock` PK; pruning uses referenced side |
| `history` → `customer` / `district` | see below | yes with care |

**`history` partition key:** use `h_w_id` (payment / home warehouse). Runtime
only inserts history; measurement never selects it. Loader CASCADE from
`warehouse` / `district` / `customer` may touch rows whose `h_c_w_id` differs
from `h_w_id` (15% remote payments). CASCADE delete from a remote customer
then has to find history rows by `h_c_w_id` **without** the partition key —
PostgreSQL may scan all history partitions for that delete. That cost hits
**load/reload**, not the measurement mix. Acceptable for v1; document it.

If FK maintenance on partitioned tables proves too expensive at load time,
expose `database.options.foreign_keys` (bool, default `true`) analogous to
OceanBase’s optional FKs, and record `ForeignKeys` in capabilities. Prefer
keeping FKs on for conformance/debugging unless benchmarks show a clear loss.

## 6. Code touch points (implementation plan)

All changes stay under `tpcc/dbms/pgsql/` (+ docs / thin CLI wiring). No edits
to protected infrastructure trees (`build/`, `contrib/`, `devtools/`,
`library/`, `util/`).

| Step | Files | Change |
| --- | --- | --- |
| 1 | `init.cpp` / `init.h` | Generate partitioned DDL from `warehouseCount`; CREATE TABLE … PARTITION BY; CREATE PARTITION OF … |
| 2 | `pg_admin_adapter.*` | Pass warehouse count into `EnsureSchema` / `InitSync` |
| 3 | `app/pgsql/main.cpp`, `worker_loader.cpp` | Require warehouses for `schema` / schema role (same as import) |
| 4 | New `partition_splitter.cpp` (or shared with YDB later) | Bounds calculation |
| 5 | `run_config.cpp` | Accept `database.options.partitioning` = `none` \| `warehouse_range` (default `warehouse_range` once implemented, or `none` until validated) |
| 6 | `pg_capabilities.cpp` | `PartitioningStyle = "warehouse_range"` when enabled |
| 7 | `path_checker.cpp` / clean | Treat partitions as part of parent; `DROP TABLE … CASCADE` already drops partitions |
| 8 | `docs/adapter-api.md` §5.1, `specification.md` §11 | Document PG warehouse RANGE |
| 9 | Tests | Schema smoke test: create with W partitions, verify `pg_partition_tree`, run a short workload or `EXPLAIN` checks for pruning |

Suggested rollout:

1. **Flag-gated DDL** (`options.partitioning=warehouse_range`) with default
   `none` until soak-tested.
2. Verify partition pruning with `EXPLAIN (ANALYZE, BUFFERS)` on Stock-Level,
   Delivery oldest-new-order, Payment-by-name, New-Order stock RMW.
3. Flip default to `warehouse_range` for orchestrated large-scale runs.
4. Only then consider district subpartitioning (§7).

## 7. Alternatives considered

### 7.1. HASH by warehouse

Pros: no bound list; easy attach of new warehouses. Cons: weaker locality for
contiguous loader ranges; diverges from YDB; harder to reason about “N WH per
shard”. Rejected for v1; keep as escape hatch if RANGE bound management becomes
painful.

### 7.2. LIST one partition per warehouse

Pros: perfect pruning. Cons: catalog bloat and planning cost at W ≥ 10k.
Rejected except possibly for tiny demo scales.

### 7.3. Subpartition by district (`RANGE` WH + `LIST` d_id)

Pros: Delivery/Stock-Level locality within a warehouse; even smaller local
indexes. Cons: 10× partition count; `stock` has no district; Payment remote
customer still crosses WH. Defer until warehouse RANGE is proven and W is so
large that per-WH indexes are still hot.

### 7.4. Partition `item` by `i_id`

YDB does this. For PostgreSQL, 100 000 rows is small; New-Order does point
PK lookups. Skip unless catalog contention appears (unlikely).

### 7.5. Partition `warehouse` / `district`

Little benefit (few rows). Keeping them unpartitioned simplifies the FK root
for CASCADE reload.

## 8. Expected pruning behavior (main scenario)

| Query class | Predicate | Expected partitions touched |
| --- | --- | --- |
| Point PK (customer, stock, order, …) | `*_w_id = $1` (+ more) | 1 |
| Customer by name | `c_w_id,c_d_id,c_last` | 1 |
| Latest order | `o_w_id,o_d_id,o_c_id` | 1 |
| Oldest new_order | `no_w_id,no_d_id` | 1 |
| Order lines by order | `ol_w_id,ol_d_id,ol_o_id` | 1 |
| Stock-Level join | `ol_w_id` + `s_w_id` (same home) | 1 + 1 |
| New-Order remote stock | `s_w_id = remote` | 1 (different partition, still point) |
| Payment remote customer | `c_w_id = remote` | 1 |
| Item lookup | unpartitioned | n/a |

No measurement query should plan as “Append of all partitions” for these
tables when `enable_partition_pruning` is on (default).

## 9. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Schema created with wrong W | Require warehouses at schema time; refuse `partitioning=warehouse_range` without positive scale |
| Scale increased after schema | Document as unsupported for RANGE v1; clean + recreate (TPC-C fixed scale) |
| FK / CASCADE load slowdown | Optional `foreign_keys=false`; measure COPY+DELETE paths |
| Planner still scans all partitions | Add regression `EXPLAIN` tests; ensure params are typed (no coercion hiding consts) |
| COPY into parent | Supported for partitioned tables; keep current `COPY` path; verify per major PG version used in CI |
| Too many partitions | Cap via warehouses-per-partition heuristic; document max recommended W |

## 10. Success criteria

1. `PartitioningStyle` reported as `warehouse_range` when enabled.
2. For W ≥ 500, Stock-Level and Delivery show single-partition prune in
   `EXPLAIN`.
3. Logical checks / consistency checks still pass with FKs enabled.
4. Idempotent warehouse reload still works via CASCADE.
5. No shared-layer transaction code changes.

## 11. Summary recommendation

Implement **RANGE partitioning by warehouse id** on
`stock`, `customer`, `oorder`, `new_order`, `order_line`, and `history`; leave
`item`, `warehouse`, and `district` unpartitioned; size partitions with the
existing YDB-style MB/warehouse heuristic; keep current PKs and secondary
indexes; gate with `database.options.partitioning` until validated, then align
capabilities with YDB’s `warehouse_range`.
