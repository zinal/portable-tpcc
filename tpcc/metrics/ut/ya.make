GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    histogram_ut.cpp
    prom_histogram_ut.cpp
)

PEERDIR(
    tpcc/metrics
)

END()
