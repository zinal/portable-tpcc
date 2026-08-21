GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    password_secret_ut.cpp
    thread_override_ut.cpp
    inflight_stuck_ut.cpp
)

PEERDIR(
    tpcc/harness
)

END()
