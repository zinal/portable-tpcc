LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/metrics
)

SRCS(
    histogram.cpp
)

END()

RECURSE_FOR_TESTS(
    ut
)
