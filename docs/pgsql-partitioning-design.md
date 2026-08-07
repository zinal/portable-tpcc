# PostgreSQL TPC-C table partitioning design

This document proposes physical partitioning for the PostgreSQL adapter
(`tpcc/dbms/pgsql`) to improve large-scale TPC-C performance while keeping
partition pruning aligned with the main workload queries and enabling useful
local indexes.

Status: **implemented** behind `database.options.partitioning=warehouse_hash`
(default remains `none`).
Related contract: [adapter-api.md](adapter-api.md) §5 (adapters MAY add
partitions), [specification.md](specification.md) §11.

## 1. Goals

1. Reduce bloat and contention on large tables (`stock`, `customer`,
   `order_line`, `oorder`, `new_order`, `history`) at high warehouse counts.
2. Ensure every main-scenario query that touches a partitioned table supplies
   an equality predicate on the partition key so PostgreSQL can prune to one
   partition — no full partition fan-out in the hot path.
3. Keep primary keys and secondary indexes meaningful **inside** each
   partition (local B-trees stay compact and selective).
4. Preserve logical TPC-C semantics, loader idempotency
   (`DELETE warehouse … CASCADE` + `COPY`), and optional foreign keys.
5. Keep schema setup simple: choose a partition **count**, not a list of
   warehouse id bounds.
6. Record the physical choice in capabilities / run settings
   (`PartitioningStyle`).

Non-goals for the first iteration:

- Changing shared transaction logic or semantic ops.
- Resharding (changing modulus) without clean + recreate.
- Subpartitioning by district (see §7).

## 2. Workload access patterns (why warehouse key)

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
an **equality** filter on `*_w_id` / `w_id`.

Equality on the partition key is exactly what PostgreSQL hash partition
pruning needs: each such lookup touches **one** partition.

## 3. Recommended scheme: HASH by warehouse id

### 3.1. Why HASH over RANGE

| | HASH | RANGE |
| --- | --- | --- |
| Schema input | single integer: modulus `N` | explicit `FROM … TO …` bound list |
| Bound generation | none | must derive splits from `W` and per-table sizing |
| Pruning on `w_id = $1` | yes (one partition) | yes (one partition) |
| New warehouses within same modulus | no DDL change | may miss coverage without DEFAULT / recreate |
| Contiguous WH ranges co-located | no | yes |
| Alignment with YDB splitter | different mechanism, same key axis | closer to YDB `warehouse_range` |

For the TPC-C mix, pruning quality is the same: every hot path uses equality on
warehouse id. HASH wins on operational simplicity — no bound generator, no
catch-all / `MAXVALUE` policy, no failure mode when `W` at load exceeds the
bounds chosen at schema time (as long as modulus stays fixed).

Loader warehouse ranges are no longer disk-contiguous; that is acceptable:
load uses `COPY` per warehouse with CASCADE delete by `w_id`, not range scans
across adjacent warehouses.

### 3.2. Style

| Setting | Value |
| --- | --- |
| Method | Declarative **`PARTITION BY HASH (warehouse_id)`** |
| Capability string | `warehouse_hash` |
| Schema knob | `database.options.partition_count` (modulus `N`) |
| Optional helper | derive `N` from `scale.warehouses` when count is omitted |
| `item` | **Not partitioned** (global, point lookup by `i_id`) |
| `warehouse`, `district` | **Not partitioned** in v1 (tiny; simplify FK roots) |

### 3.3. Tables to partition

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
is accessed via `stock.s_w_id`, which is hashed correctly on its own.

Use the **same modulus `N`** for all partitioned tables so planning and ops
stay uniform.

### 3.4. Choosing `N` (partition count)

Only `N` must be decided at schema time. Suggested policy:

1. If `database.options.partition_count` is set → use it (must be ≥ 1).
2. Else if `scale.warehouses` / `--warehouses` is known → derive `N` from a
   simple size heuristic (optional convenience, not required for correctness):
   - Target roughly ~2 GiB per partition using `stock` ≈ 45 MB/WH as the
     dominant table → about 45 warehouses per partition.
   - Floor: `max(16, ceil(W / 100))` (avoid tiny modulus on large `W`).
   - Cap: e.g. 1024 (catalog / planning cost).
3. Else → reject schema with partitioning enabled (need either explicit
   `partition_count` or warehouses).

Same `N` for every partitioned table. Powers of two are fine but **not**
required by PostgreSQL.

Example: `W = 1000` → ~45 WH/partition → `N ≈ 23` (or round to 32 if a
power-of-two preference is added later).

Changing `N` later requires clean + recreate (hash modulus is part of the
physical layout). Document that; TPC-C scale is normally fixed for a run.

### 3.5. DDL sketch

```sql
CREATE TABLE stock (
    s_w_id int NOT NULL,
    s_i_id int NOT NULL,
    -- … columns …
    PRIMARY KEY (s_w_id, s_i_id)
) PARTITION BY HASH (s_w_id);

CREATE TABLE stock_p0 PARTITION OF stock
    FOR VALUES WITH (MODULUS 32, REMAINDER 0);
-- … remainder 1 .. 31 …
```

Generation is a trivial loop `for r in 0 .. N-1`, with no bound arithmetic.

Primary keys already contain the partition column, satisfying PostgreSQL’s
rule that unique constraints on partitioned tables include the partition key.
The existing unique constraint `idx_order (o_w_id, o_d_id, o_c_id, o_id)` is
likewise valid.

`InitSync` needs the modulus (and optionally warehouses only to derive it):

```text
TPgAdminAdapter(connection, path, partitionCount)
  → InitSync(connection, path, partitionCount)
```

CLI `tpcc schema` accepts `--partition_count` and/or `--warehouses` when
`partitioning=warehouse_hash`.

## 4. Indexes after partitioning

Partitioning does not replace indexes; it makes them local and smaller.

| Index | Definition | Role after partitioning |
| --- | --- | --- |
| PK per table | unchanged, warehouse-leading | Local unique index per partition; prune by hash then seek |
| `idx_order` | `UNIQUE (o_w_id, o_d_id, o_c_id, o_id)` | Order-Status “latest order for customer”; still local |
| `idx_customer_name` | `(c_w_id, c_d_id, c_last, c_first)` | Payment / Order-Status by last name; prune on `c_w_id` then local scan |

Optional follow-ups (not required for correctness):

1. **Delivery** — PK `(no_w_id, no_d_id, no_o_id)` already matches
   `WHERE no_w_id=$1 AND no_d_id=$2 ORDER BY no_o_id LIMIT 1 FOR UPDATE`.
   Hash pruning shrinks the leaf pages; no extra index expected.
2. **Stock-Level** — PK prefix on `order_line` + point PK on `stock`; both
   prune on warehouse equality. Avoid indexes that ignore `*_w_id`.
3. Keep leading `*_w_id` in secondary index definitions on the parent table
   (documents the access path used by shared SQL).

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

| From | To | OK with warehouse HASH? |
| --- | --- | --- |
| `stock(s_w_id)` → `warehouse(w_id)` | unpartitioned parent | yes |
| `stock(s_i_id)` → `item(i_id)` | unpartitioned | yes |
| `district` → `warehouse` | both unpartitioned | yes |
| `customer` → `district` | partitioned → unpartitioned | yes |
| `oorder` → `customer` | both hashed on same WH key | yes (keys align) |
| `new_order` → `oorder` | same | yes |
| `order_line` → `oorder` | same home WH | yes |
| `order_line(ol_supply_w_id, ol_i_id)` → `stock` | supply WH may ≠ home WH | yes: FK columns are exactly `stock` PK |
| `history` → `customer` / `district` | see below | yes with care |

**`history` partition key:** use `h_w_id` (payment / home warehouse). Runtime
only inserts history; measurement never selects it. Loader CASCADE from a
remote customer (`h_c_w_id`) may scan all history partitions when deleting —
load/reload cost only. Acceptable for v1; document it.

Optional `database.options.foreign_keys` (bool / `"on"`|`"off"`, default
`true` / `"on"`) omits FOREIGN KEY clauses at schema time, analogous to
OceanBase. CLI: `--foreign_keys=off`. Capabilities record `ForeignKeys`.
Idempotent warehouse reload uses explicit per-table deletes so load works
with either setting.

## 6. Implementation status

Implemented under `tpcc/dbms/pgsql/` (+ CLI / harness document fields / docs).
No edits to protected infrastructure trees (`build/`, `contrib/`, `devtools/`,
`library/`, `util/`).

| Area | Location |
| --- | --- |
| Config / derive `N` | `partition_config.{h,cpp}` |
| DDL | `init.cpp` (`BuildTpccSchemaDdl`) |
| Admin / schema role | `pg_admin_adapter.*`, `worker_loader.cpp` |
| CLI | `app/pgsql/main.cpp` (`--partitioning`, `--partition-count`, `--foreign_keys`) |
| run-config / tpccctl | `database.options.partitioning`, `partition_count`, `foreign_keys` (profile → run-config.json → schema role) |
| Capabilities | `TPgCapabilities` reports configured style |
| Preflight | `path_checker.cpp` recognizes `PARTITIONED TABLE` |
| Unit tests | `tpcc/dbms/pgsql/ut/partition_config_ut.cpp` |

Rollout:

1. **Flag-gated DDL** (`options.partitioning=warehouse_hash`) with default
   `none` until soak-tested. ✅
2. Verify partition pruning with `EXPLAIN (ANALYZE, BUFFERS)` on Stock-Level,
   Delivery oldest-new-order, Payment-by-name, New-Order stock RMW.
3. Flip default to `warehouse_hash` for orchestrated large-scale runs.
4. Only then consider district subpartitioning (§7).

## 7. Alternatives considered

### 7.1. RANGE by warehouse

Pros: contiguous WH ranges co-located; closer to YDB `PARTITION_AT_KEYS`.
Cons: must generate and maintain explicit bounds; schema depends tightly on
`W`; easy to get “no partition for row” if scale grows. Rejected for v1 in
favor of HASH simplicity; revisit only if measured locality of contiguous
ranges matters for a specific PG deployment.

### 7.2. LIST one partition per warehouse

Pros: perfect pruning. Cons: catalog bloat and planning cost at W ≥ 10k.
Rejected except possibly for tiny demo scales.

### 7.3. Subpartition by district

Pros: even smaller local indexes for Delivery/Stock-Level. Cons: 10×
partition count; `stock` has no district. Defer until warehouse HASH is proven.

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
| New-Order remote stock | `s_w_id = remote` | 1 (possibly another remainder) |
| Payment remote customer | `c_w_id = remote` | 1 |
| Item lookup | unpartitioned | n/a |

No measurement query should plan as “Append of all partitions” for these
tables when `enable_partition_pruning` is on (default). Hash pruning requires
an equality (or `IS NULL`) constraining the hash key — which the mix already
has.

## 9. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Bad / missing `N` | Require `partition_count` or warehouses when hashing enabled |
| Skew (uneven WH per remainder) | Accept statistical skew; TPC-C WH ids are dense integers — hash distributes well enough |
| Changing `N` after load | Unsupported; clean + recreate |
| FK / CASCADE load slowdown | Optional `foreign_keys=false`; measure COPY+DELETE paths |
| Planner still scans all partitions | `EXPLAIN` regression tests; keep params typed as `int` |
| COPY into parent | Supported; verify on the PG major used in CI |
| Too large `N` | Cap (e.g. 1024); document guidance |

## 10. Success criteria

1. `PartitioningStyle` reported as `warehouse_hash` when enabled.
2. For W ≥ 500 with sensible `N`, Stock-Level and Delivery show
   single-partition prune in `EXPLAIN`.
3. Logical checks / consistency checks still pass with FKs enabled.
4. Idempotent warehouse reload still works via CASCADE.
5. Schema path does not generate warehouse id ranges — only modulus/remainder
   children.
6. No shared-layer transaction code changes.

## 11. Summary recommendation

Implement **HASH partitioning by warehouse id** on
`stock`, `customer`, `oorder`, `new_order`, `order_line`, and `history`; leave
`item`, `warehouse`, and `district` unpartitioned; configure with a single
`partition_count` (optionally derived from `W`); keep current PKs and
secondary indexes; gate with `database.options.partitioning=warehouse_hash`
until validated.
