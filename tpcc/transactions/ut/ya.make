GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    session_ut.cpp
    error_classifier_ut.cpp
)

PEERDIR(
    tpcc/transactions
)

END()
