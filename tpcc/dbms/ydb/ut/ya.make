GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    ydb_future_ut.cpp
    clock_calibration_ut.cpp
    ydb_value_parse_ut.cpp
    ydb_driver_ut.cpp
    arrow_upsert_ut.cpp
    load_batch_ut.cpp
    ydb_error_classifier_ut.cpp
)

PEERDIR(
    tpcc/dbms/ydb
)

END()
