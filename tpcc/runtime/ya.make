LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/runtime
)

SRCS(
    task_queue.cpp
)

PEERDIR(
    tpcc/metrics
)

CFLAGS(
    -DTPCC_NO_SPDLOG
)

END()
