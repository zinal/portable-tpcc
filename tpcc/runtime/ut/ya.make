GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    time_util_ut.cpp
    phase_controller_ut.cpp
    warehouse_range_ut.cpp
    timer_queue_ut.cpp
    future_util_ut.cpp
)

PEERDIR(
    tpcc/runtime
)

END()
