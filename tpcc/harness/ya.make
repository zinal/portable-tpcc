LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/harness
)

SRCS(
    terminal.cpp
    artifacts.cpp
    clock_skew.cpp
    sha256.cpp
    run_loop.cpp
    orchestrated_roles.cpp
)

PEERDIR(
    tpcc/domain
    tpcc/metrics
    tpcc/runtime
    tpcc/transactions
    contrib/libs/fmt
    contrib/restricted/nlohmann_json
    contrib/libs/openssl
)

END()
