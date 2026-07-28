LIBRARY()

SRCS(
    server.cpp
)

PEERDIR(
    devtools/executor/proc_util
    devtools/executor/net
    devtools/executor/proc_info
    devtools/executor/proto
    library/cpp/sighandler
    library/cpp/deprecated/atomic
)

END()
