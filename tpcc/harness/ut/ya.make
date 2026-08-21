GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    password_secret_ut.cpp
    thread_override_ut.cpp
)

PEERDIR(
    tpcc/harness
)

END()
