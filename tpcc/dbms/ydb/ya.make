LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/dbms/ydb
)

SRCS(
    ydb_driver.cpp
    ydb_error_classifier.cpp
    ydb_capabilities.cpp
    ydb_session.cpp
    init.cpp
    data_splitter.cpp
    import.cpp
    load_batch.cpp
    clean.cpp
    check.cpp
    ydb_admin_adapter.cpp
    path_checker.cpp
    runner.cpp
    run_config.cpp
    clock_calibration.cpp
    worker_loader.cpp
)

PEERDIR(
    tpcc/domain
    tpcc/generator
    tpcc/loader
    tpcc/checks
    tpcc/transactions
    tpcc/metrics
    tpcc/runtime
    tpcc/harness
    ydb/public/api/grpc
    ydb/public/api/protos
    ydb/public/sdk/cpp/src/client/driver
    ydb/public/sdk/cpp/src/client/query
    ydb/public/sdk/cpp/src/client/table
    ydb/public/sdk/cpp/src/client/scheme
    ydb/public/sdk/cpp/src/client/operation
    ydb/public/sdk/cpp/src/client/proto
    ydb/public/sdk/cpp/src/client/iam
    ydb/public/sdk/cpp/src/client/types/credentials
    ydb/public/sdk/cpp/src/client/types/credentials/login
    contrib/libs/fmt
    contrib/restricted/nlohmann_json
    library/cpp/logger
)

END()

RECURSE_FOR_TESTS(
    ut
)
