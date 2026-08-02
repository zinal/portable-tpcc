GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    histogram_ut.cpp
)

PEERDIR(
    tpcc/metrics
)

END()
