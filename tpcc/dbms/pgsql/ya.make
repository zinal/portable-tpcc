LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/dbms/pgsql
)

SRCS(
    pg_session.cpp
    pg_connection_pool.cpp
    common_queries.cpp
    transaction_neworder.cpp
    transaction_delivery.cpp
    transaction_orderstatus.cpp
    transaction_payment.cpp
    transaction_stocklevel.cpp
    transaction_simulation.cpp
    init.cpp
    import.cpp
    load_batch.cpp
    clean.cpp
    check.cpp
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
    worker_loader.cpp
)

PEERDIR(
    tpcc/domain
    tpcc/generator
    tpcc/loader
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
