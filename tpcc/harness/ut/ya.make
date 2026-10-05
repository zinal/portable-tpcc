GTEST()

SUBSCRIBER(g:tpcc)

SRCS(
    password_secret_ut.cpp
    module_commit_ut.cpp
    thread_override_ut.cpp
    inflight_stuck_ut.cpp
    artifact_manifest_stdio_ut.cpp
    debug_probe_ut.cpp
    latency_constraints_ut.cpp
    progress_line_ut.cpp
    terminal_start_ut.cpp
    prometheus_export_ut.cpp
    interrupt_result_ut.cpp
)

PEERDIR(
    tpcc/harness
    contrib/restricted/nlohmann_json
)

END()
