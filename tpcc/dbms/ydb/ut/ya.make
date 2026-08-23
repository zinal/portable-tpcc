GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    ydb_future_ut.cpp
    clock_calibration_ut.cpp
)

PEERDIR(
    tpcc/dbms/ydb
)

END()
