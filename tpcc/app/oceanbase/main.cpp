#include "check.h"
#include "clean.h"
#include "import.h"
#include "init.h"
#include "ob_admin_adapter.h"
#include "path_checker.h"
#include "runner.h"
#include "schema_options.h"
#include "worker_loader.h"

#include <domain_util.h>
#include <log.h>
#include <think_time.h>

#include <library/cpp/logger/priority.h>

#include <gflags/gflags.h>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

DEFINE_string(connection, "host=127.0.0.1;port=2881;user=root@test;password=tpcc;database=tpcc",
              "OceanBase connection string (supports query_timeout=<seconds>, default 600)");
DEFINE_string(path, "", "OceanBase database name for benchmark tables (default: connection database)");
DEFINE_int32(partitions, 0, "OceanBase hash partitions: -1 = off, 0 = derive from --warehouses, N = explicit");
DEFINE_string(foreign_keys, "on", "Foreign keys: on or off");

DEFINE_int32(warehouses, 1, "Number of warehouses");
DEFINE_uint64(seed, 1, "Deterministic data generation seed");
DEFINE_int32(warmup, 0, "Warmup duration in minutes (0 = adaptive)");
DEFINE_bool(skip_warmup, false, "Skip warmup entirely and start measurement immediately");
DEFINE_int32(duration, 10, "Benchmark run duration in minutes");
DEFINE_int32(threads, 0, "Number of threads (coroutines for run, importers for import); 0 = auto");
DEFINE_int32(max_inflight, NTpcc::DEFAULT_MAX_INFLIGHT, "Max inflight transactions");
DEFINE_bool(no_delays, false, "Disable keying and think time delays");
DEFINE_string(think_time_distribution, "exponential",
    "Think time distribution: exponential (TPC-C default) or compatibility/constant");
DEFINE_bool(high_res_histogram, false, "Use high resolution histograms");
DEFINE_int32(simulate_select1, 0, "Simulation mode: run N SELECT 1 queries per transaction instead of real TPC-C (0 = disabled)");
DEFINE_string(log_level, "info", "Log level: trace, debug, info, warn, error");
DEFINE_bool(after_import, false, "Check mode: verify freshly loaded data (stricter invariants)");
DEFINE_bool(after_run, false, "Check mode: verify data after a measurement run");

namespace {

void PrintHelp() {
    std::cout <<
        "tpcc-oceanbase - TPC-C benchmark for OceanBase\n"
        "\n"
        "Usage: tpcc-oceanbase <command> [options]\n"
        "\n"
        "Commands:\n"
        "  schema    Create TPC-C schema (tables); alias: init\n"
        "  loader    Run orchestrated loader from run-config.json\n"
        "  worker    Run orchestrated worker from run-config.json\n"
        "  check     Run TPC-C consistency checks\n"
        "  init      Alias for schema\n"
        "  import    Load TPC-C data (standalone)\n"
        "  run       Run the TPC-C benchmark (standalone)\n"
        "  clean     Drop all TPC-C tables (local admin)\n"
        "\n"
        "Options:\n"
        "  --connection          OceanBase connection string\n"
        "                        (query_timeout=<seconds> for bulk/ANALYZE; default 600)\n"
        "  -p, --path            OceanBase database name for benchmark tables\n"
        "  --partitions          -1 off, 0 derive from -w, N explicit (default: 0)\n"
        "  --foreign-keys        on | off (default: on)\n"
        "  -w, --warehouses      Number of warehouses (default: 1)\n"
        "  --warmup              Warmup duration in minutes, 0 = adaptive (default: 0)\n"
        "  --skip-warmup         Skip warmup entirely (default: false)\n"
        "  --duration            Benchmark run duration in minutes (default: 10)\n"
        "  -t, --threads         Number of threads; 0 = auto (default: 0)\n"
        "  -m, --max-inflight    Max inflight transactions (default: 100)\n"
        "  --no-delays           Disable keying and think time delays (default: false)\n"
        "  --think-time-distribution  exponential, compatibility, or constant\n"
        "  --high-res-histogram  Use high resolution histograms (default: false)\n"
        "  --log-level           Log level: trace, debug, info, warn, error\n"
        "  --after-import        check: verify freshly loaded data\n"
        "  --after-run           check: verify data after a measurement run\n"
        "\n"
        "Orchestrated mode (mind-tpcc):\n"
        "  schema --run-config <path> --instance <name>\n"
        "  loader --run-config <path> --instance <name>\n"
        "  worker --run-config <path> --instance <name> --start-at=<RFC3339-UTC>\n"
        "  check  --run-config <path> --instance <name> --after-import|--after-run\n"
        "  clean  --run-config <path> --instance <name>\n";
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
    return cmd == "schema" || cmd == "init" || cmd == "import" || cmd == "run" ||
           cmd == "worker" || cmd == "loader" || cmd == "clean" || cmd == "check";
}

bool IsOrchestratedRole(const std::string& cmd) {
    return cmd == "worker" || cmd == "loader" || cmd == "schema" || cmd == "check" || cmd == "clean";
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
    if (FLAGS_duration <= 0) {
        throw std::runtime_error("--duration must be greater than zero");
    }
    if (FLAGS_warmup < 0) {
        throw std::runtime_error("--warmup must not be negative");
    }
}

bool ParseOrchestratedArgs(
    int argc,
    char** argv,
    std::string& runConfig,
    std::string& instance,
    std::optional<std::string>& startAt,
    bool& afterImport,
    bool& afterRun)
{
    afterImport = false;
    afterRun = false;
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
        } else if (arg == "--after-run" || arg == "--after_run") {
            afterRun = true;
        }
    }
    return !runConfig.empty() && !instance.empty();
}

int RunOrchestrated(
    const std::string& command,
    const std::string& runConfig,
    const std::string& instance,
    const std::optional<std::string>& startAt,
    bool afterImport,
    bool afterRun)
{
    if (command == "worker") return NTpcc::RunWorkerFromRunConfig(runConfig, instance, startAt);
    if (command == "loader") return NTpcc::RunLoaderFromRunConfig(runConfig, instance);
    if (command == "schema") return NTpcc::RunSchemaFromRunConfig(runConfig, instance);
    if (command == "check") return NTpcc::RunCheckFromRunConfig(runConfig, instance, afterImport, afterRun);
    if (command == "clean") return NTpcc::RunCleanFromRunConfig(runConfig, instance);
    return 1;
}

const char* ShortFlagToLong(char c) {
    switch (c) {
        case 'w': return "warehouses";
        case 't': return "threads";
        case 'm': return "max_inflight";
        case 'p': return "path";
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

NTpcc::TObSchemaOptions BuildSchemaOptions() {
    bool foreignKeys = true;
    if (!NTpcc::ParseForeignKeysMode(FLAGS_foreign_keys, foreignKeys)) {
        throw std::runtime_error("--foreign-keys must be on or off");
    }
    NTpcc::TObSchemaOptions options;
    options.PartitionCount = FLAGS_partitions;
    options.WarehouseCount = FLAGS_warehouses;
    options.EnableForeignKeys = foreignKeys;
    NTpcc::ResolveObPartitionCount(options);
    return options;
}

void RunSchema() {
    ValidateWarehouseFlag();
    auto options = BuildSchemaOptions();
    NTpcc::CheckDbForInit(FLAGS_connection, FLAGS_path);
    NTpcc::TObAdminAdapter admin(FLAGS_connection, FLAGS_path, options);
    admin.EnsureSchema();
}

void RunImport() {
    ValidateWarehouseFlag();
    ValidateThreadsFlag();
    NTpcc::CheckDbForImport(FLAGS_connection, FLAGS_path);
    NTpcc::TImportConfig config;
    config.ConnectionString = FLAGS_connection;
    config.Path = FLAGS_path;
    config.WarehouseCount = FLAGS_warehouses;
    config.LoadThreadCount = FLAGS_threads;
    config.Seed = FLAGS_seed;
    NTpcc::ImportSync(config);
}

void RunBenchmark() {
    ValidateRunFlags();
    auto options = BuildSchemaOptions();
    NTpcc::TRunConfig config;
    config.ConnectionString = FLAGS_connection;
    config.Path = FLAGS_path;
    config.Partitioning = NTpcc::ObPartitioningStyle(options);
    config.ForeignKeys = options.EnableForeignKeys;
    config.WarehouseCount = FLAGS_warehouses;
    config.WarmupDuration = std::chrono::minutes(FLAGS_warmup);
    config.RunDuration = std::chrono::minutes(FLAGS_duration);
    config.SkipWarmup = FLAGS_skip_warmup;
    config.ThreadCount = FLAGS_threads;
    config.MaxInflight = FLAGS_max_inflight;
    config.NoDelays = FLAGS_no_delays;
    config.HighResHistogram = FLAGS_high_res_histogram;
    config.SimulateTransactionSelect1 = FLAGS_simulate_select1;
    if (!NTpcc::ParseThinkTimeDistribution(FLAGS_think_time_distribution, config.ThinkTimeDistribution)) {
        throw std::runtime_error("--think-time-distribution must be exponential, compatibility, or constant");
    }
    if (!config.IsSimulationMode()) {
        NTpcc::CheckDbForRun(FLAGS_connection, FLAGS_warehouses, FLAGS_path);
    }
    NTpcc::RunSync(config, nullptr);
}

void RunClean() {
    NTpcc::TObAdminAdapter admin(FLAGS_connection, FLAGS_path);
    admin.Clean();
}

void RunCheck() {
    ValidateWarehouseFlag();
    if (FLAGS_after_import && FLAGS_after_run) {
        throw std::runtime_error("specify only one of --after-import or --after-run");
    }
    const bool afterImport = FLAGS_after_import;
    NTpcc::CheckDbForRun(FLAGS_connection, FLAGS_warehouses, FLAGS_path);
    NTpcc::CheckSync(FLAGS_connection, FLAGS_warehouses, afterImport, FLAGS_path);
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
            if (ParseOrchestratedArgs(argc, argv, runConfig, instance, startAt, afterImport, afterRun)) {
                if (earlyCommand == "worker" && !startAt.has_value()) {
                    std::cerr << "Error: worker requires --start-at=<RFC3339-UTC>\n";
                    return 1;
                }
                if (earlyCommand == "check" && afterImport == afterRun) {
                    std::cerr << "Error: check requires exactly one of --after-import or --after-run\n";
                    return 1;
                }
                NTpcc::InitLogging(TLOG_INFO);
                try {
                    return RunOrchestrated(earlyCommand, runConfig, instance, startAt, afterImport, afterRun);
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
        return 1;
    }

    gflags::SetUsageMessage("TPC-C benchmark for OceanBase");
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
        }
    } catch (const std::exception& ex) {
        LOG_E("Fatal error: " << ex.what());
        return 1;
    }

    return NTpcc::GetGlobalErrorVariable().load() ? 1 : 0;
}
