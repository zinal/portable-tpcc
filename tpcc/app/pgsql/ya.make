PROGRAM(tpcc-pgsql)

SUBSCRIBER(g:tpcc)

SRCS(
    main.cpp
)

PEERDIR(
    tpcc/dbms/pgsql
    contrib/libs/gflags
    contrib/restricted/spdlog
)

END()
