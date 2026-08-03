LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/dbms/oceanbase
)

SRCS(
    ob_queries.cpp
    ob_prepared_statement.cpp
    ob_connection.cpp
    ob_session.cpp
    ob_connection_pool.cpp
    ob_error_classifier.cpp
    ob_capabilities.cpp
    schema_options.cpp
    tpcc_session.cpp
    init.cpp
    import.cpp
    load_batch.cpp
    clean.cpp
    check.cpp
    ob_admin_adapter.cpp
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
    contrib/restricted/obconnector-c
    contrib/libs/fmt
    contrib/restricted/nlohmann_json
    library/cpp/logger
)

END()
