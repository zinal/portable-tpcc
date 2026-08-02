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

Implementation status and third-party dependencies:
[docs/dependencies.md](docs/dependencies.md). Shared libraries and adapter API:
[docs/adapter-api.md](docs/adapter-api.md). Alignment plan (accepted API
decisions and phase checklist):
[docs/alignment-plan.md](docs/alignment-plan.md). Engineering vs TPC-C 5.11
conformance notes:
[docs/tpcc-5.11-conformance-analysis.md](docs/tpcc-5.11-conformance-analysis.md).

Results MUST NOT be called official TPC-C results without the required TPC
verification.
