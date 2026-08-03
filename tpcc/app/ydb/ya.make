PROGRAM(tpcc-ydb)

SUBSCRIBER(g:tpcc)

SRCS(
    main.cpp
)

PEERDIR(
    tpcc/dbms/ydb
    contrib/libs/gflags
    library/cpp/logger
)

END()
