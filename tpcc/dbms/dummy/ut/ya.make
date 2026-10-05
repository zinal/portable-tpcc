GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    dummy_delay_ut.cpp
    run_config_ut.cpp
    dummy_session_ut.cpp
    check_ut.cpp
)

PEERDIR(
    tpcc/dbms/dummy
    tpcc/harness
)

END()
