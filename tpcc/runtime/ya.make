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
    library/cpp/logger
    library/cpp/colorizer
)

END()

RECURSE_FOR_TESTS(
    ut
)
