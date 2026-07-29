# portable-tpcc

A horizontally scalable TPC-C implementation with shared workload logic,
YDB/PostgreSQL/OceanBase adapters, and a dedicated orchestrator.

Architecture draft:

- [specification](docs/specification.md);
- [profile example](docs/examples/profile.v1.yaml);
- [run-config example](docs/examples/run-config.v1.json);
- [start-token example](docs/examples/start-token.v1.json);
- [aggregate example](docs/examples/aggregate.v1.json).

Implementation has started. The initial PostgreSQL port (`tpcc-pgsql`) and
shared libraries are described in [docs/dependencies.md](docs/dependencies.md).

Results MUST NOT be called official TPC-C results without the required TPC
verification.
