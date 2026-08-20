GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    schema_options_ut.cpp
)

PEERDIR(
    tpcc/dbms/oceanbase
)

END()
