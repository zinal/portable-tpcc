LIBRARY()

SRCS(
    ydb_issue_message.cpp
)

PEERDIR(
    ydb/public/api/protos
)

END()

RECURSE_FOR_TESTS(
    ut
)

