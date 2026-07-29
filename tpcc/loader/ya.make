LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/loader
)

PEERDIR(
    tpcc/domain
)

END()

RECURSE_FOR_TESTS(
    ut
)
