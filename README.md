# portable-tpcc

A horizontally scalable TPC-C implementation with shared workload logic,
YDB/PostgreSQL/OceanBase adapters, and a dedicated orchestrator.

Architecture draft:

- [specification](docs/specification.md);
- [shared libraries and adapter API](docs/adapter-api.md);
- [profile example](docs/examples/profile.v1.yaml);
- [run-config example](docs/examples/run-config.v1.json);
- [start-token example](docs/examples/start-token.v1.json);
- [aggregate example](docs/examples/aggregate.v1.json).

Implementation has started. The initial PostgreSQL port (`tpcc-pgsql`) and
shared libraries are described in [docs/dependencies.md](docs/dependencies.md)
and [docs/adapter-api.md](docs/adapter-api.md). A gap analysis versus the design
documents is in
[docs/implementation-gap-analysis.md](docs/implementation-gap-analysis.md);
the alignment plan (with accepted API decisions) is in
[docs/alignment-plan.md](docs/alignment-plan.md).

Results MUST NOT be called official TPC-C results without the required TPC
verification.
