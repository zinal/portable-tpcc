GTEST()

SUBSCRIBER(g:tpcc)

ADDINCL(
    tpcc/dbms/ydb
)

SRCS(
    scheme_path_ut.cpp
)

END()
