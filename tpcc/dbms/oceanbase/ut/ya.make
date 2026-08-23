GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    load_batch_ut.cpp
    schema_options_ut.cpp
)

PEERDIR(
    tpcc/dbms/oceanbase
)

END()
