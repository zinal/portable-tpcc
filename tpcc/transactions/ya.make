LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/transactions
)

SRCS(
    error_classifier.cpp
    session.cpp
    context.cpp
    new_order.cpp
    payment.cpp
    delivery.cpp
    order_status.cpp
    stock_level.cpp
)

PEERDIR(
    tpcc/domain
    tpcc/runtime
)

END()

RECURSE_FOR_TESTS(
    ut
)
