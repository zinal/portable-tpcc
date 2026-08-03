PROGRAM(tpcc-oceanbase)

SUBSCRIBER(g:tpcc)

SRCS(
    main.cpp
)

PEERDIR(
    tpcc/dbms/oceanbase
    contrib/libs/gflags
    library/cpp/logger
)

END()
