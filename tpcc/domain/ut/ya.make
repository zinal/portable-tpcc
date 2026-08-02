GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    money_ut.cpp
    workload_config_ut.cpp
)

PEERDIR(
    tpcc/domain
)

END()
