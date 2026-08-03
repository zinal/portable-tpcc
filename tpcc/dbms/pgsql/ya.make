LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/dbms/pgsql
)

SRCS(
    pg_session.cpp
    pg_connection_pool.cpp
    init.cpp
    import.cpp
    load_batch.cpp
    partition_config.cpp
    pg_error_classifier.cpp
    pg_capabilities.cpp
    tpcc_session.cpp
    clean.cpp
    check.cpp
    pg_admin_adapter.cpp
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
    contrib/libs/libpqxx
    contrib/libs/fmt
    contrib/restricted/nlohmann_json
    library/cpp/logger
)

END()

RECURSE_FOR_TESTS(
    ut
)
