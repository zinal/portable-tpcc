GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    ydb_future_ut.cpp
)

PEERDIR(
    tpcc/dbms/ydb
)

END()
