LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/runtime
)

SRCS(
    task_queue.cpp
    log_backend.cpp
    time_util.cpp
)

PEERDIR(
    tpcc/metrics
    contrib/restricted/spdlog
)

END()

RECURSE_FOR_TESTS(
    ut
)
