GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    catalog_ut.cpp
)

PEERDIR(
    tpcc/checks
)

END()
