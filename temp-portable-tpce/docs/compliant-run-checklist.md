# Compliant multi-instance run checklist

Use this checklist before a declared compliant TPC-E fair-use run with horizontal scaling. Operational orchestration (`tpcectl`) is described in [`spec-orchestrator.md`](spec-orchestrator.md); runtime contracts are in [`spec-scalability.md`](spec-scalability.md).

## Topology

- [ ] Exactly **one** DM-capable Driver process (`--role dm`, or a single `--role standalone`).
- [ ] All other Driver processes use `--role ce` only.
- [ ] Every MEE instance has a **unique** `-U` / `unique_id`.
- [ ] CE `UniqueId` intervals `[ce-id-base, ce-id-base + u)` do not overlap between CE processes.
- [ ] All processes use the **same** global run-config (`scale.*`, `paths.data`, `endpoint_sets`, `base_time_epoch`).
- [ ] No external L4/L7 load balancer; BH/MEE pools are in-process only.

## CE partitioning (if used)

- [ ] All three flags set together, or partitioning disabled entirely.
- [ ] `ce-part-percent = 50` for a compliant run.
- [ ] `ce-start-id % 1000 == 1`.
- [ ] `ce-part-count >= 5000` and `ce-part-count % 1000 == 0`.
- [ ] Partition subrange fits within configured customers (`-t`).

## Bootstrap order

1. [ ] PostgreSQL is up with schema and data loaded.
2. [ ] `BH₁ … BHₖ` started; each reaches **service-ready** (`--ready-file`).
3. [ ] `MEE₁ … MEEₘ` started; each reaches **service-ready** after `SetBaseTime()`.
4. [ ] `DM` started only after all BH/MEE are service-ready.
5. [ ] `CE₁ … CEₙ` started only after DM Trade-Cleanup completes.

## Timing and seeds

- [ ] `base_time_epoch` is sufficiently in the future at MEE startup.
- [ ] Manual multi-MEE: `epoch >= now + pool_init_timeout + 5s`.
- [ ] No manual RNG seed (`-r`) on Driver for a valid run.

## Database capacity

- [ ] PostgreSQL `max_connections` (and any pooler limits) cover **all concurrent BH worker connections**. See [`db-connection-sizing.md`](db-connection-sizing.md).

## Prohibited configurations

Reject or do not document as compliant:

- Multiple active DM processes.
- Partition percent other than 50 in a compliant run.
- Partition size below 5000 or misaligned to load units.
- Duplicate MEE `UniqueId` values.
- Packet-content routing or external VIP instead of application pools.
