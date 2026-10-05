PROGRAM(tpcc-dummy)

SUBSCRIBER(g:tpcc)

SRCS(
    main.cpp
)

PEERDIR(
    tpcc/dbms/dummy
    contrib/libs/gflags
    library/cpp/logger
)

END()
