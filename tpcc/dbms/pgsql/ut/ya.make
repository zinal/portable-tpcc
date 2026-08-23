GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    load_batch_ut.cpp
    partition_config_ut.cpp
    run_config_ut.cpp
)

PEERDIR(
    tpcc/dbms/pgsql
)

END()
