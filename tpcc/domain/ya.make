LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/domain
)

SRCS(
    domain_util.cpp
)

PEERDIR(
    util
)

END()

RECURSE_FOR_TESTS(
    ut
)
