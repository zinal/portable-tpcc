GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    password_secret_ut.cpp
    thread_override_ut.cpp
    inflight_stuck_ut.cpp
    artifact_manifest_stdio_ut.cpp
    debug_probe_ut.cpp
    latency_constraints_ut.cpp
)

PEERDIR(
    tpcc/harness
    contrib/restricted/nlohmann_json
)

END()
