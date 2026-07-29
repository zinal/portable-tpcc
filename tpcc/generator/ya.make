LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/generator
)

SRCS(
    populate.cpp
)

PEERDIR(
    tpcc/domain
)

END()

RECURSE_FOR_TESTS(
    ut
)
