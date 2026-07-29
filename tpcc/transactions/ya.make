LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/transactions
)

SRCS(
    error_classifier.cpp
)

PEERDIR(
    tpcc/domain
    tpcc/runtime
)

END()

RECURSE_FOR_TESTS(
    ut
)
