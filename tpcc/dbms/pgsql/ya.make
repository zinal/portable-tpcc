LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/dbms/pgsql
)

SRCS(
    pg_session.cpp
    pg_connection_pool.cpp
    transaction_simulation.cpp
    init.cpp
    import.cpp
    load_batch.cpp
    pg_error_classifier.cpp
    pg_capabilities.cpp
    tpcc_session.cpp
    clean.cpp
    check.cpp
    pg_admin_adapter.cpp
    path_checker.cpp
    terminal.cpp
    runner.cpp
    runner_tui.cpp
    import_tui.cpp
    tui_base.cpp
    scroller.cpp
    logs_scroller.cpp
    warehouse_range.cpp
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
    contrib/libs/libpqxx
    contrib/libs/fmt
    contrib/restricted/spdlog
    contrib/libs/ftxui
    contrib/restricted/nlohmann_json
    contrib/libs/openssl
)

CFLAGS(
    -DTPCC_HAS_TUI=1
)

END()
