# portable-tpcc

A horizontally scalable TPC-C implementation with shared workload logic,
YDB/PostgreSQL/OceanBase adapters, and a dedicated orchestrator.

This repository currently contains the architecture draft:

- [portable-tpcc specification](docs/specification.md);
- [orchestrator profile example](docs/examples/profile.v1.yaml);
- [control-plane configuration example](docs/examples/control-config.v1.json);
- [normalized run configuration example](docs/examples/run-config.v1.json);
- [synchronized start-token example](docs/examples/start-token.v1.json).

Implementation has not started. Results produced by the future software MUST
NOT be called official TPC-C results without the required TPC verification.
