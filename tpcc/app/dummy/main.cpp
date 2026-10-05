#include "import.h"
#include "runner.h"
#include "check.h"
#include "dummy_admin_adapter.h"
#include "dummy_delay.h"
#include "worker_loader.h"

#include <log.h>
#include <domain_util.h>
#include <think_time.h>
#include <debug_probe.h>

#include <library/cpp/logger/priority.h>

#include <gflags/gflags.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

DEFINE_string(connection, "",
    "Dummy connection: delay_us_min=N delay_us_max=M (microseconds, inclusive)");
DEFINE_int64(delay_us_min, 0, "Minimum simulated DBMS delay in microseconds");
DEFINE_int64(delay_us_max, 0, "Maximum simulated DBMS delay in microseconds");

DEFINE_int32(warehouses, 1, "Number of warehouses");
DEFINE_uint64(seed, 1, "Deterministic data generation seed");
DEFINE_int32(warmup, 0, "Warmup duration in minutes (0 = adaptive)");
DEFINE_bool(skip_warmup, false, "Skip warmup entirely and start measurement immediately");
DEFINE_int32(duration, 10, "Benchmark run duration in minutes");
DEFINE_int32(threads, 0,
    "Number of threads (coroutines for run, importers for import, parallel DB sessions for check); "
    "0 = auto for run/import, serial for check");
DEFINE_int32(max_inflight, NTpcc::DEFAULT_MAX_INFLIGHT,
    "Max in-flight transactions");
DEFINE_int32(stats_interval, NTpcc::kDefaultStatsIntervalSeconds,
    "Seconds between worker progress statistics lines");
DEFINE_int32(metrics_port, 0,
    "Prometheus metrics listen port for the run command (0 = disabled)");
DEFINE_bool(no_delays, false, "Disable keying and think time delays");
DEFINE_string(think_time_distribution, "exponential",
    "Think time distribution: exponential (TPC-C default) or compatibility/constant");
DEFINE_bool(high_res_histogram, false, "Use high resolution histograms");
DEFINE_int32(simulate_select1, 0, "Simulation mode: run N SELECT 1 queries per transaction instead of real TPC-C (0 = disabled)");
DEFINE_string(log_level, "info", "Log level: trace, debug, info, warn, error");
DEFINE_bool(after_import, false, "Check mode: verify freshly loaded data (stricter invariants)");
DEFINE_bool(after_test, false, "Check mode: verify data after the measurement test");
DEFINE_bool(after_run, false, "Deprecated alias for --after-test");
DEFINE_int32(repeats, NTpcc::kDefaultDebugRepeats,
    "Debug: sequential executions of each TPC-C transaction type");

namespace {

void PrintHelp() {
    std::cout <<
        "tpcc-dummy - TPC-C benchmark against a simulated DBMS\n"
        "\n"
        "Usage: tpcc-dummy <command> [options]\n"
        "\n"
        "The dummy adapter never talks to a real database. Schema, load, indexes,\n"
        "drop, and checks are empty and succeed. The worker uses the shared TPC-C\n"
        "terminal, phase, workflow, and metrics stack; each adapter round-trip waits\n"
        "a random duration in [delay_us_min, delay_us_max] microseconds.\n"
        "\n"
        "Commands (normative roles):\n"
        "  schema    Create TPC-C schema (no-op); alias: init\n"
        "  loader    Run orchestrated loader from run-config.json\n"
        "  indexes   Create secondary indexes (no-op)\n"
        "  worker    Run orchestrated worker from run-config.json\n"
        "  check     Run TPC-C consistency checks (always pass)\n"
        "  debug     Sequential probe: each transaction type 10 times (plan timings)\n"
        "\n"
        "Legacy / local aliases:\n"
        "  init      ≡ schema\n"
        "  import    Load TPC-C data (no-op)\n"
        "  run       Run the TPC-C benchmark (standalone)\n"
        "  drop      Drop TPC-C tables (no-op)\n"
        "\n"
        "Options:\n"
        "  --connection          Dummy connection string: delay_us_min=N delay_us_max=M\n"
        "  --delay-us-min        Minimum simulated delay in microseconds (default: 0)\n"
        "  --delay-us-max        Maximum simulated delay in microseconds (default: 0)\n"
        "  -w, --warehouses      Number of warehouses (default: 1)\n"
        "  --warmup              Warmup duration in minutes, 0 = adaptive (default: 0)\n"
        "  --skip-warmup         Skip warmup entirely (default: false)\n"
        "  --duration            Benchmark run duration in minutes (default: 10)\n"
        "  -t, --threads         Number of threads (coroutines for run, importers for import,\n"
        "                        parallel sessions for check); 0 = auto for run/import,\n"
        "                        serial (1 session) for check (default: 0)\n"
        "  -m, --max-inflight    Max in-flight transactions (default: 100)\n"
        "  --stats-interval      Seconds between progress statistics lines (default: 30)\n"
        "  --metrics-port        Prometheus /metrics listen port for run (0 = off, default: 0)\n"
        "  --no-delays           Disable keying and think time delays (default: false)\n"
        "  --think-time-distribution  exponential (TPC-C default) or compatibility/constant\n"
        "  --high-res-histogram  Use high resolution histograms (default: false)\n"
        "  --log-level           Log level: trace, debug, info, warn, error (default: \"info\")\n"
        "  --after-import        check: after-import catalog (always pass)\n"
        "  --after-test          check: after-test catalog (always pass)\n"
        "  --after-run           deprecated alias for --after-test\n"
        "  --repeats             debug: executions per transaction type (default: 10)\n"
        "\n"
        "Orchestrated mode (mind-tpcc):\n"
        "  schema  --run-config <path> --instance <name>\n"
        "  loader  --run-config <path> --instance <name> [--threads=N]\n"
        "  indexes --run-config <path> --instance <name>\n"
        "  worker  --run-config <path> --instance <name> --start-at=<RFC3339-UTC> [--threads=N] [--max-inflight=N] [--metrics-port=N]\n"
        "  check   --run-config <path> --instance <name> --after-import|--after-test [--threads=N]\n"
        "  debug   --run-config <path> --instance <name> [--repeats=N]\n"
        "  drop    --run-config <path> --instance <name>\n"
        "\n"
        "Examples:\n"
        "  tpcc-dummy schema\n"
        "  tpcc-dummy import -w 10\n"
        "  tpcc-dummy run -w 10 --duration=1 --no-delays --delay-us-min=100 --delay-us-max=500\n"
        "  tpcc-dummy check -w 10 --after-test\n"
        "  tpcc-dummy debug -w 10\n";
}

ELogPriority ParseLogLevel(const std::string& level) {
    if (level == "trace") return TLOG_RESOURCES;
    if (level == "debug") return TLOG_DEBUG;
    if (level == "info") return TLOG_INFO;
    if (level == "warn" || level == "warning") return TLOG_WARNING;
    if (level == "error" || level == "err") return TLOG_ERR;
    return TLOG_INFO;
}

bool IsValidCommand(const std::string& cmd) {
    return cmd == "schema" || cmd == "init" || cmd == "import" || cmd == "indexes" ||
           cmd == "run" || cmd == "worker" || cmd == "loader" || cmd == "drop" || cmd == "check" ||
           cmd == "debug";
}

bool IsOrchestratedRole(const std::string& cmd) {
    return cmd == "worker" || cmd == "loader" || cmd == "schema" || cmd == "indexes" ||
           cmd == "check" || cmd == "drop" || cmd == "debug";
}

void ValidateWarehouseFlag() {
    if (FLAGS_warehouses <= 0) {
        throw std::runtime_error("--warehouses must be greater than zero");
    }
}

void ValidateThreadsFlag() {
    if (FLAGS_threads < 0) {
        throw std::runtime_error("--threads must not be negative");
    }
}

void ValidateRunFlags() {
    ValidateWarehouseFlag();
    ValidateThreadsFlag();
    if (FLAGS_max_inflight <= 0) {
        throw std::runtime_error("--max-inflight must be greater than zero");
    }
    if (FLAGS_stats_interval <= 0) {
        throw std::runtime_error("--stats-interval must be greater than zero");
    }
    if (FLAGS_metrics_port < 0 || FLAGS_metrics_port > 65535) {
        throw std::runtime_error("--metrics-port must be between 0 and 65535");
    }
    if (FLAGS_duration <= 0) {
        throw std::runtime_error("--duration must be greater than zero");
    }
    if (FLAGS_warmup < 0) {
        throw std::runtime_error("--warmup must not be negative");
    }
}

NTpcc::TDummyDelayConfig DelayFromFlags() {
    if (!FLAGS_connection.empty()) {
        return NTpcc::ParseDummyConnection(FLAGS_connection);
    }
    NTpcc::TDummyDelayConfig delay;
    delay.MinUs = FLAGS_delay_us_min;
    delay.MaxUs = FLAGS_delay_us_max;
    NTpcc::ValidateDummyDelayConfig(delay);
    return delay;
}

bool ParseOrchestratedArgs(
    int argc,
    char** argv,
    std::string& runConfig,
    std::string& instance,
    std::optional<std::string>& startAt,
    bool& afterImport,
    bool& afterRun,
    std::optional<int>& threads,
    std::optional<int>& repeats,
    std::optional<int>& maxInflight,
    std::optional<int>& metricsPort)
{
    afterImport = false;
    afterRun = false;
    threads.reset();
    repeats.reset();
    maxInflight.reset();
    metricsPort.reset();
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--run-config" && i + 1 < argc) {
            runConfig = argv[++i];
        } else if (arg == "--instance" && i + 1 < argc) {
            instance = argv[++i];
        } else if (arg.rfind("--start-at=", 0) == 0) {
            startAt = arg.substr(std::string("--start-at=").size());
        } else if (arg == "--start-at" && i + 1 < argc) {
            startAt = argv[++i];
        } else if (arg == "--after-import" || arg == "--after_import") {
            afterImport = true;
        } else if (arg == "--after-test" || arg == "--after_test" ||
                   arg == "--after-run" || arg == "--after_run") {
            afterRun = true;
        } else if (arg.rfind("--threads=", 0) == 0) {
            threads = std::stoi(arg.substr(std::string("--threads=").size()));
        } else if ((arg == "--threads" || arg == "-t") && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (arg.rfind("--max-inflight=", 0) == 0) {
            maxInflight = std::stoi(arg.substr(std::string("--max-inflight=").size()));
        } else if (arg.rfind("--max_inflight=", 0) == 0) {
            maxInflight = std::stoi(arg.substr(std::string("--max_inflight=").size()));
        } else if ((arg == "--max-inflight" || arg == "--max_inflight" || arg == "-m") && i + 1 < argc) {
            maxInflight = std::stoi(argv[++i]);
        } else if (arg.rfind("--metrics-port=", 0) == 0) {
            metricsPort = std::stoi(arg.substr(std::string("--metrics-port=").size()));
        } else if (arg.rfind("--metrics_port=", 0) == 0) {
            metricsPort = std::stoi(arg.substr(std::string("--metrics_port=").size()));
        } else if ((arg == "--metrics-port" || arg == "--metrics_port") && i + 1 < argc) {
            metricsPort = std::stoi(argv[++i]);
        } else if (arg.rfind("--repeats=", 0) == 0) {
            repeats = std::stoi(arg.substr(std::string("--repeats=").size()));
        } else if (arg == "--repeats" && i + 1 < argc) {
            repeats = std::stoi(argv[++i]);
        }
    }
    return !runConfig.empty() && !instance.empty();
}

int RunOrchestratedSchema(const std::string& runConfig, const std::string& instance) {
    return NTpcc::RunSchemaFromRunConfig(runConfig, instance);
}

int RunOrchestrated(
    const std::string& command,
    const std::string& runConfig,
    const std::string& instance,
    const std::optional<std::string>& startAt,
    bool afterImport,
    bool afterRun,
    const std::optional<int>& threads,
    const std::optional<int>& repeats,
    const std::optional<int>& maxInflight,
    const std::optional<int>& metricsPort)
{
    if (threads.has_value() && *threads < 0) {
        throw std::runtime_error("--threads must not be negative");
    }
    if (repeats.has_value() && *repeats <= 0) {
        throw std::runtime_error("--repeats must be greater than zero");
    }
    if (maxInflight.has_value() && *maxInflight <= 0) {
        throw std::runtime_error("--max-inflight must be greater than zero");
    }
    if (metricsPort.has_value() && (*metricsPort <= 0 || *metricsPort > 65535)) {
        throw std::runtime_error("--metrics-port must be between 1 and 65535");
    }
    if (command == "worker") {
        LOG_I("Starting orchestrated worker " << instance << "...");
        return NTpcc::RunWorkerFromRunConfig(runConfig, instance, startAt, threads, maxInflight, metricsPort);
    }
    if (command == "loader") {
        LOG_I("Starting orchestrated loader " << instance << "...");
        return NTpcc::RunLoaderFromRunConfig(runConfig, instance, threads);
    }
    if (command == "schema") {
        LOG_I("Starting orchestrated schema " << instance << "...");
        return RunOrchestratedSchema(runConfig, instance);
    }
    if (command == "indexes") {
        LOG_I("Starting orchestrated indexes " << instance << "...");
        return NTpcc::RunIndexesFromRunConfig(runConfig, instance);
    }
    if (command == "check") {
        const int checkConcurrency = (!threads.has_value() || *threads <= 0) ? 1 : *threads;
        LOG_I("Starting orchestrated check " << instance
              << " (concurrency=" << checkConcurrency << ")...");
        return NTpcc::RunCheckFromRunConfig(
            runConfig, instance, afterImport, afterRun, checkConcurrency);
    }
    if (command == "debug") {
        const int n = repeats.value_or(NTpcc::kDefaultDebugRepeats);
        LOG_I("Starting orchestrated debug " << instance << " (repeats=" << n << ")...");
        return NTpcc::RunDebugFromRunConfig(runConfig, instance, n);
    }
    if (command == "drop") {
        LOG_I("Starting orchestrated drop " << instance << "...");
        return NTpcc::RunDropFromRunConfig(runConfig, instance);
    }
    return 1;
}

const char* ShortFlagToLong(char c) {
    switch (c) {
        case 'w': return "warehouses";
        case 't': return "threads";
        case 'm': return "max_inflight";
        default: return nullptr;
    }
}

std::string PreprocessArgs(
    int& argc,
    char**& argv,
    std::vector<std::string>& storage,
    std::vector<char*>& argvStorage)
{
    std::string subcommand;

    storage.reserve(argc);
    argvStorage.reserve(argc + 1);
    argvStorage.push_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        char* arg = argv[i];

        if (arg[0] != '-') {
            if (subcommand.empty()) {
                subcommand = arg;
            } else {
                argvStorage.push_back(arg);
            }
            continue;
        }

        if (arg[1] == '-') {
            argvStorage.push_back(arg);
            continue;
        }

        char shortChar = arg[1];
        const char* longName = ShortFlagToLong(shortChar);
        if (longName == nullptr) {
            argvStorage.push_back(arg);
            continue;
        }

        if (arg[2] == '\0') {
            storage.emplace_back(std::string("--") + longName);
        } else if (arg[2] == '=') {
            storage.emplace_back(std::string("--") + longName + (arg + 2));
        } else {
            storage.emplace_back(std::string("--") + longName + "=" + (arg + 2));
        }
        argvStorage.push_back(storage.back().data());
    }

    argvStorage.push_back(nullptr);
    argc = static_cast<int>(argvStorage.size()) - 1;
    argv = argvStorage.data();
    return subcommand;
}

void RunSchema() {
    NTpcc::TDummyAdminAdapter admin;
    admin.EnsureSchema();
}

void RunImport() {
    ValidateWarehouseFlag();
    ValidateThreadsFlag();
    NTpcc::TImportConfig config;
    config.WarehouseCount = FLAGS_warehouses;
    config.Seed = FLAGS_seed;
    NTpcc::ImportSync(config);
}

void RunIndexes() {
    NTpcc::TDummyAdminAdapter admin;
    admin.EnsureIndexes();
    admin.EnsureStatistics();
}

void RunBenchmark() {
    ValidateRunFlags();
    NTpcc::TRunConfig config;
    config.Delay = DelayFromFlags();
    config.WarehouseCount = FLAGS_warehouses;
    config.WarmupDuration = std::chrono::minutes(FLAGS_warmup);
    config.RunDuration = std::chrono::minutes(FLAGS_duration);
    config.SkipWarmup = FLAGS_skip_warmup;
    config.ThreadCount = FLAGS_threads;
    config.MaxInflight = FLAGS_max_inflight;
    config.StatsInterval = std::chrono::seconds(FLAGS_stats_interval);
    config.MetricsPort = FLAGS_metrics_port;
    config.NoDelays = FLAGS_no_delays;
    config.HighResHistogram = FLAGS_high_res_histogram;
    config.SimulateTransactionSelect1 = FLAGS_simulate_select1;
    if (!NTpcc::ParseThinkTimeDistribution(FLAGS_think_time_distribution, config.ThinkTimeDistribution)) {
        throw std::runtime_error(
            "--think-time-distribution must be \"exponential\", \"compatibility\", or \"constant\"");
    }
    NTpcc::RunSync(config, nullptr);
}

void RunDrop() {
    NTpcc::TDummyAdminAdapter admin;
    admin.Clean();
}

void RunCheck() {
    ValidateWarehouseFlag();
    ValidateThreadsFlag();
    if (FLAGS_after_import && (FLAGS_after_test || FLAGS_after_run)) {
        throw std::runtime_error("specify only one of --after-import or --after-test");
    }
    const bool afterImport = FLAGS_after_import;
    const int checkConcurrency = FLAGS_threads <= 0 ? 1 : FLAGS_threads;
    NTpcc::CheckSync(FLAGS_warehouses, afterImport, checkConcurrency);
}

void RunDebug() {
    ValidateWarehouseFlag();
    if (FLAGS_repeats <= 0) {
        throw std::runtime_error("--repeats must be greater than zero");
    }
    NTpcc::DebugSync(DelayFromFlags(), FLAGS_warehouses, FLAGS_repeats);
}

} // anonymous

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-help" || arg == "--helpshort" || arg == "-h") {
            PrintHelp();
            return 0;
        }
    }

    if (argc >= 2) {
        const std::string earlyCommand = argv[1];
        if (IsOrchestratedRole(earlyCommand)) {
            std::string runConfig;
            std::string instance;
            std::optional<std::string> startAt;
            bool afterImport = false;
            bool afterRun = false;
            std::optional<int> threads;
            std::optional<int> repeats;
            std::optional<int> maxInflight;
            std::optional<int> metricsPort;
            const bool hasOrchestrated = ParseOrchestratedArgs(
                argc, argv, runConfig, instance, startAt, afterImport, afterRun, threads, repeats, maxInflight, metricsPort);
            if (hasOrchestrated) {
                if (earlyCommand == "worker" && !startAt.has_value()) {
                    std::cerr << "Error: worker requires --start-at=<RFC3339-UTC>\n";
                    return 1;
                }
                if (earlyCommand == "check" && afterImport == afterRun) {
                    std::cerr << "Error: check requires exactly one of --after-import or --after-test\n";
                    return 1;
                }
                NTpcc::InitLogging(TLOG_INFO);
                try {
                    return RunOrchestrated(
                        earlyCommand, runConfig, instance, startAt, afterImport, afterRun, threads, repeats, maxInflight, metricsPort);
                } catch (const std::exception& ex) {
                    LOG_E("Fatal error: " << ex.what());
                    return 1;
                }
            }
            if (earlyCommand == "worker" || earlyCommand == "loader") {
                std::cerr << "Error: worker/loader require --run-config and --instance\n";
                return 1;
            }
        }
    }

    std::vector<std::string> argStorage;
    std::vector<char*> argvStorage;
    std::string command = PreprocessArgs(argc, argv, argStorage, argvStorage);

    if (command.empty()) {
        std::cerr << "Error: no command specified\n\n";
        PrintHelp();
        return 1;
    }

    if (!IsValidCommand(command)) {
        std::cerr << "Unknown command: " << command << "\n";
        std::cerr << "Valid commands: schema, init, import, indexes, run, worker, loader, drop, check, debug\n";
        return 1;
    }

    gflags::SetUsageMessage("TPC-C benchmark against a simulated DBMS");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    NTpcc::InitLogging(ParseLogLevel(FLAGS_log_level));

    try {
        if (command == "schema" || command == "init") {
            LOG_I("Initializing TPC-C schema...");
            RunSchema();
            LOG_I("Schema initialization complete");
        } else if (command == "import") {
            LOG_I("Importing TPC-C data (" << FLAGS_warehouses << " warehouses)...");
            RunImport();
            LOG_I("Data import complete");
        } else if (command == "indexes") {
            LOG_I("Creating secondary indexes and gathering statistics...");
            RunIndexes();
            LOG_I("Indexes and statistics ready");
        } else if (command == "run") {
            LOG_I("Running TPC-C benchmark...");
            RunBenchmark();
        } else if (command == "drop") {
            LOG_I("Dropping TPC-C tables...");
            RunDrop();
            LOG_I("Drop complete");
        } else if (command == "check") {
            LOG_I("Running TPC-C consistency checks...");
            RunCheck();
            LOG_I("Consistency checks complete");
        } else if (command == "debug") {
            LOG_I("Running sequential transaction debug probe...");
            RunDebug();
            LOG_I("Debug probe complete");
        } else if (command == "worker" || command == "loader") {
            std::string runConfig;
            std::string instance;
            std::optional<std::string> startAt;
            bool afterImport = false;
            bool afterRun = false;
            std::optional<int> threads;
            std::optional<int> repeats;
            std::optional<int> maxInflight;
            std::optional<int> metricsPort;
            if (!ParseOrchestratedArgs(
                    argc, argv, runConfig, instance, startAt, afterImport, afterRun, threads, repeats, maxInflight, metricsPort)) {
                std::cerr << "Error: worker/loader require --run-config and --instance\n";
                return 1;
            }
            if (command == "worker" && !startAt.has_value()) {
                std::cerr << "Error: worker requires --start-at=<RFC3339-UTC>\n";
                return 1;
            }
            return RunOrchestrated(
                command, runConfig, instance, startAt, afterImport, afterRun, threads, repeats, maxInflight, metricsPort);
        }
    } catch (const std::exception& ex) {
        LOG_E("Fatal error: " << ex.what());
        return 1;
    }

    return NTpcc::GetGlobalErrorVariable().load() ? 1 : 0;
}
