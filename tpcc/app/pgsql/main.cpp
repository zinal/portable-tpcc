#include "init.h"
#include "import.h"
#include "runner.h"
#include "clean.h"
#include "check.h"
#include "path_checker.h"
#include "worker_loader.h"

#include <log.h>
#include <domain_util.h>

#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

DEFINE_string(connection, "host=localhost dbname=tpcc user=postgres", "PostgreSQL connection string");
DEFINE_string(path, "", "PostgreSQL schema for benchmark tables (default: empty, uses server search_path)");

DEFINE_int32(warehouses, 1, "Number of warehouses");
DEFINE_uint64(seed, 1, "Deterministic data generation seed");
DEFINE_int32(warmup, 0, "Warmup duration in minutes (0 = adaptive)");
DEFINE_bool(skip_warmup, false, "Skip warmup entirely and start measurement immediately");
DEFINE_int32(duration, 10, "Benchmark run duration in minutes");
DEFINE_int32(threads, 0, "Number of threads (coroutines for run, importers for import); 0 = auto");
DEFINE_int32(max_inflight, NTpcc::DEFAULT_MAX_INFLIGHT, "Max inflight transactions");
DEFINE_bool(no_delays, false, "Disable keying and think time delays");
DEFINE_bool(high_res_histogram, false, "Use high resolution histograms");
DEFINE_int32(simulate_ms, 0, "Simulation mode: sleep N ms per transaction instead of real TPC-C (0 = disabled)");
DEFINE_int32(simulate_select1, 0, "Simulation mode: run N SELECT 1 queries per transaction instead of real TPC-C (0 = disabled)");
DEFINE_string(log_level, "info", "Log level: trace, debug, info, warn, error");
DEFINE_bool(no_tui, false, "Disable terminal UI");
DEFINE_bool(after_import, false, "Check mode: verify freshly loaded data (stricter invariants)");

namespace {

void PrintHelp() {
    std::cout <<
        "tpcc - TPC-C benchmark for PostgreSQL\n"
        "\n"
        "Usage: tpcc <command> [options]\n"
        "\n"
        "Commands:\n"
        "  init      Create TPC-C schema (tables, indexes)\n"
        "  import    Load TPC-C data into the database\n"
        "  run       Run the TPC-C benchmark\n"
        "  worker    Run orchestrated worker from run-config.json\n"
        "  loader    Run orchestrated loader from run-config.json\n"
        "  clean     Drop all TPC-C tables\n"
        "  check     Run TPC-C consistency checks\n"
        "\n"
        "Options:\n"
        "  --connection          PostgreSQL connection string\n"
        "                        (default: \"host=localhost dbname=tpcc user=postgres\")\n"
        "  -p, --path            PostgreSQL schema for benchmark tables (default: empty, uses server search_path)\n"
        "  -w, --warehouses      Number of warehouses (default: 1)\n"
        "  --warmup              Warmup duration in minutes, 0 = adaptive (default: 0)\n"
        "  --skip-warmup         Skip warmup entirely (default: false)\n"
        "  --duration            Benchmark run duration in minutes (default: 10)\n"
        "  -t, --threads         Number of threads (coroutines for run, importers for import);\n"
        "                        0 = auto (default: 0)\n"
        "  -m, --max-inflight    Max inflight transactions (default: 100)\n"
        "  --no-delays           Disable keying and think time delays (default: false)\n"
        "  --high-res-histogram  Use high resolution histograms (default: false)\n"
        "  --log-level           Log level: trace, debug, info, warn, error (default: \"info\")\n"
        "  --no-tui              Disable terminal UI (default: false)\n"
        "  --after-import        check: verify freshly loaded data (default: false)\n"
        "\n"
        "Orchestrated mode (tpccctl):\n"
        "  worker --run-config <path> --instance <name> --start-at=<RFC3339-UTC>\n"
        "  loader --run-config <path> --instance <name>\n"
        "\n"
        "Simulation (for testing without real TPC-C transactions):\n"
        "  --simulate-ms         Sleep N ms per transaction (default: 0 = disabled)\n"
        "  --simulate-select1    Run N SELECT 1 queries per transaction (default: 0 = disabled)\n"
        "\n"
        "Examples:\n"
        "  tpcc init --connection=\"host=localhost dbname=tpcc\"\n"
        "  tpcc import -w 10 -t 8\n"
        "  tpcc run -w 10 --duration=5 -t 4\n"
        "  tpcc check -w 10\n"
        "  tpcc check -w 10 --after-import\n";
}

spdlog::level::level_enum ParseLogLevel(const std::string& level) {
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "info") return spdlog::level::info;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error" || level == "err") return spdlog::level::err;
    return spdlog::level::info;
}

bool IsValidCommand(const std::string& cmd) {
    return cmd == "init" || cmd == "import" || cmd == "run" || cmd == "worker" ||
           cmd == "loader" || cmd == "clean" || cmd == "check";
}

bool ParseOrchestratedArgs(
    int argc,
    char** argv,
    std::string& runConfig,
    std::string& instance,
    std::optional<std::string>& startAt)
{
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
        }
    }
    return !runConfig.empty() && !instance.empty();
}

int RunOrchestrated(
    const std::string& command,
    const std::string& runConfig,
    const std::string& instance,
    const std::optional<std::string>& startAt)
{
    if (command == "worker") {
        LOG_I("Starting orchestrated worker {}...", instance);
        return NTpcc::RunWorkerFromRunConfig(runConfig, instance, startAt);
    }
    if (command == "loader") {
        LOG_I("Starting orchestrated loader {}...", instance);
        return NTpcc::RunLoaderFromRunConfig(runConfig, instance);
    }
    return 1;
}

// Maps a short flag character to its long-form gflags name (using
// underscores, as gflags stores them internally).
const char* ShortFlagToLong(char c) {
    switch (c) {
        case 'w': return "warehouses";
        case 't': return "threads";
        case 'm': return "max_inflight";
        case 'p': return "path";
        default: return nullptr;
    }
}

// Pre-process argv to:
//   1. Extract the first positional argument as the subcommand.
//   2. Rewrite short flags (-w/-t/-m, optionally with =VAL) to their long
//      gflags form so gflags itself never sees the short form.
//
// Both argc and argv are modified in place. Returns the extracted subcommand
// (empty if none was found). Allocated rewrites are stored in `storage` and
// the new argv pointer array in `argvStorage`; both must outlive gflags
// parsing.
std::string PreprocessArgs(
    int& argc,
    char**& argv,
    std::vector<std::string>& storage,
    std::vector<char*>& argvStorage)
{
    std::string subcommand;

    // Reserve enough so std::string buffers don't get reallocated and
    // invalidate the c_str() pointers we hand to gflags.
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

        // Long flag (--foo, --foo=bar) or "--": pass through.
        if (arg[1] == '-') {
            argvStorage.push_back(arg);
            continue;
        }

        // Single-char flag candidate: -X or -X=VAL or -XVAL.
        char shortChar = arg[1];
        const char* longName = ShortFlagToLong(shortChar);
        if (longName == nullptr) {
            // Unknown short flag (e.g. -h): pass through to gflags.
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

void RunInit() {
    NTpcc::CheckDbForInit(FLAGS_connection, FLAGS_path);
    NTpcc::InitSync(FLAGS_connection, FLAGS_path);
}

void RunImport() {
    NTpcc::CheckDbForImport(FLAGS_connection, FLAGS_path);
    NTpcc::TImportConfig config;
    config.ConnectionString = FLAGS_connection;
    config.Path = FLAGS_path;
    config.WarehouseCount = FLAGS_warehouses;
    config.LoadThreadCount = FLAGS_threads;
    config.UseTui = !FLAGS_no_tui;
    config.Seed = FLAGS_seed;
    NTpcc::ImportSync(config);
}

void RunBenchmark() {
    NTpcc::TRunConfig config;
    config.ConnectionString = FLAGS_connection;
    config.Path = FLAGS_path;
    config.WarehouseCount = FLAGS_warehouses;
    config.WarmupDuration = std::chrono::minutes(FLAGS_warmup);
    config.RunDuration = std::chrono::minutes(FLAGS_duration);
    config.SkipWarmup = FLAGS_skip_warmup;
    config.ThreadCount = FLAGS_threads;
    config.MaxInflight = FLAGS_max_inflight;
    config.NoDelays = FLAGS_no_delays;
    config.HighResHistogram = FLAGS_high_res_histogram;
    config.SimulateTransactionMs = FLAGS_simulate_ms;
    config.SimulateTransactionSelect1 = FLAGS_simulate_select1;
    config.UseTui = !FLAGS_no_tui;

    if (!config.IsSimulationMode()) {
        NTpcc::CheckDbForRun(FLAGS_connection, FLAGS_warehouses, FLAGS_path);
    }

    NTpcc::RunSync(config, nullptr);
}

void RunClean() {
    NTpcc::CleanSync(FLAGS_connection, FLAGS_path);
}

void RunCheck() {
    NTpcc::CheckDbForRun(FLAGS_connection, FLAGS_warehouses, FLAGS_path);
    NTpcc::CheckSync(FLAGS_connection, FLAGS_warehouses, FLAGS_after_import, FLAGS_path);
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
        if (earlyCommand == "worker" || earlyCommand == "loader") {
            std::string runConfig;
            std::string instance;
            std::optional<std::string> startAt;
            if (!ParseOrchestratedArgs(argc, argv, runConfig, instance, startAt)) {
                std::cerr << "Error: worker/loader require --run-config and --instance\n";
                return 1;
            }
            if (earlyCommand == "worker" && !startAt.has_value()) {
                std::cerr << "Error: worker requires --start-at=<RFC3339-UTC>\n";
                return 1;
            }
            NTpcc::InitLogging();
            spdlog::set_level(spdlog::level::info);
            try {
                return RunOrchestrated(earlyCommand, runConfig, instance, startAt);
            } catch (const std::exception& ex) {
                LOG_E("Fatal error: {}", ex.what());
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
        std::cerr << "Valid commands: init, import, run, worker, loader, clean, check\n";
        return 1;
    }

    gflags::SetUsageMessage("TPC-C benchmark for PostgreSQL");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    NTpcc::InitLogging();
    spdlog::set_level(ParseLogLevel(FLAGS_log_level));

    try {
        if (command == "init") {
            LOG_I("Initializing TPC-C schema...");
            RunInit();
            LOG_I("Schema initialization complete");
        } else if (command == "import") {
            LOG_I("Importing TPC-C data ({} warehouses)...", FLAGS_warehouses);
            RunImport();
            LOG_I("Data import complete");
        } else if (command == "run") {
            LOG_I("Running TPC-C benchmark...");
            RunBenchmark();
        } else if (command == "clean") {
            LOG_I("Cleaning TPC-C tables...");
            RunClean();
            LOG_I("Clean complete");
        } else if (command == "check") {
            LOG_I("Running TPC-C consistency checks...");
            RunCheck();
            LOG_I("Consistency checks complete");
        } else if (command == "worker" || command == "loader") {
            std::string runConfig;
            std::string instance;
            std::optional<std::string> startAt;
            if (!ParseOrchestratedArgs(argc, argv, runConfig, instance, startAt)) {
                std::cerr << "Error: worker/loader require --run-config and --instance\n";
                return 1;
            }
            if (command == "worker" && !startAt.has_value()) {
                std::cerr << "Error: worker requires --start-at=<RFC3339-UTC>\n";
                return 1;
            }
            return RunOrchestrated(command, runConfig, instance, startAt);
        }
    } catch (const std::exception& ex) {
        LOG_E("Fatal error: {}", ex.what());
        return 1;
    }

    return NTpcc::GetGlobalErrorVariable().load() ? 1 : 0;
}
