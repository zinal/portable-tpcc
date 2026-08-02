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
    transaction_simulation.cpp
    init.cpp
    data_splitter.cpp
    import.cpp
    load_batch.cpp
    clean.cpp
    check.cpp
    ydb_admin_adapter.cpp
    path_checker.cpp
    terminal.cpp
    runner.cpp
    runner_tui.cpp
    import_tui.cpp
    tui_base.cpp
    scroller.cpp
    logs_scroller.cpp
    run_config.cpp
    artifacts.cpp
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
    ydb/public/api/grpc
    ydb/public/api/protos
    ydb/public/sdk/cpp/src/client/driver
    ydb/public/sdk/cpp/src/client/query
    ydb/public/sdk/cpp/src/client/table
    ydb/public/sdk/cpp/src/client/scheme
    ydb/public/sdk/cpp/src/client/operation
    ydb/public/sdk/cpp/src/client/proto
    contrib/libs/fmt
    contrib/libs/ftxui
    contrib/restricted/nlohmann_json
    contrib/libs/openssl
    library/cpp/logger
)

CFLAGS(
    -DTPCC_HAS_TUI=1
)

END()
