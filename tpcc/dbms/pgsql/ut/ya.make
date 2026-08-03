GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    partition_config_ut.cpp
)

PEERDIR(
    tpcc/dbms/pgsql
)

END()
