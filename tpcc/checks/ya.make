LIBRARY()

SUBSCRIBER(g:tpcc)

ADDINCL(
    GLOBAL tpcc/checks
)

SRCS(
    catalog.cpp
    report.cpp
)

PEERDIR(
    contrib/restricted/nlohmann_json
)

END()

RECURSE_FOR_TESTS(
    ut
)
