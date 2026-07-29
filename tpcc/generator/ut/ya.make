GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    populate_ut.cpp
)

PEERDIR(
    tpcc/domain
    tpcc/generator
)

END()
