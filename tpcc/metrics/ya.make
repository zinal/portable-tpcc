LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/metrics
)

SRCS(
    histogram.cpp
    prom_histogram.cpp
)

END()

RECURSE_FOR_TESTS(
    ut
)
