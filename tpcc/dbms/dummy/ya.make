LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/dbms/dummy
)

SRCS(
    dummy_delay.cpp
    dummy_session.cpp
    dummy_error_classifier.cpp
    dummy_capabilities.cpp
    dummy_admin_adapter.cpp
    load_batch.cpp
    import.cpp
    check.cpp
    clock_calibration.cpp
    runner.cpp
    run_config.cpp
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
    contrib/restricted/nlohmann_json
    library/cpp/logger
)

END()

RECURSE_FOR_TESTS(
    ut
)
