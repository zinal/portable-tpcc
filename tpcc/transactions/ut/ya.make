GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    session_ut.cpp
    error_classifier_ut.cpp
    workflow_ut.cpp
    new_order_supply_ut.cpp
)

PEERDIR(
    tpcc/transactions
)

END()
