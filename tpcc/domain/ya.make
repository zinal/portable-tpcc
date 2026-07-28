LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/domain
)

SRCS(
    util.cpp
)

PEERDIR(
    util
)

END()
