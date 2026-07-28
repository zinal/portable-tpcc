# PostgreSQL connection sizing for multi-instance BH

In the client-side variant (`BrokerageHouseMain -1`), each accepted CE/DM connection spawns a BH worker thread with its **own** `libpq` session. There is no shared connection pool inside Tier-A.

## Rule of thumb

Plan PostgreSQL server capacity for:

```text
required_backend_connections >= sum(active_BH_workers) + admin_margin
```

where `active_BH_workers` is the number of concurrently connected CE/DM sessions across **all** BH instances.

If `--max-workers` is set on a BH instance, that instance contributes at most `max_workers` concurrent database sessions. Without the limit, one BH process can grow without bound as CE threads connect.

## Example

| Component | Count | Concurrent DB sessions |
| --- | --- | --- |
| BH1 (`--max-workers 64`) | 1 | up to 64 |
| BH2 (`--max-workers 64`) | 1 | up to 64 |
| CE total user threads | 128 | up to 128 driver sockets (spread across BH pool) |

In the worst case, all CE threads may occupy workers on one BH before round-robin spreads load. Size **each** BH host and PostgreSQL for the peak concurrent workers you allow, not only the average.

## Recommendations

1. Set `--max-workers` on each BH to a value your database can sustain.
2. Set PostgreSQL `max_connections` above the sum of all BH worker limits plus loader/admin sessions.
3. Use a separate `-o` output directory per BH/CE/MEE/DM instance on shared hosts.
4. Monitor `BrokerageHouse_Error.log` for worker-limit rejections when tuning.

This is operational guidance from [`spec-scalability.md`](spec-scalability.md) §9; it is not a TPC-E timing requirement.
