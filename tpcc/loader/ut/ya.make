GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    put_batch_ut.cpp
)

PEERDIR(
    tpcc/loader
)

END()
