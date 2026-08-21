GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    home_district_ut.cpp
    money_ut.cpp
    think_time_ut.cpp
    workload_config_ut.cpp
    pacing_credit_ut.cpp
)

PEERDIR(
    tpcc/domain
)

END()
