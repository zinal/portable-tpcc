GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    session_ut.cpp
)

PEERDIR(
    tpcc/transactions
)

END()
