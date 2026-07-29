GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    workload_config_ut.cpp
)

PEERDIR(
    tpcc/domain
)

END()
