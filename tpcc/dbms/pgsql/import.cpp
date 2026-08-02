#include "import.h"
#include "warehouse_range.h"
#include "load_batch.h"

#include <constants.h>
#include "init.h"
#include <log.h>
#include <domain_util.h>

#ifdef TPCC_HAS_TUI
#include "import_tui.h"
#include <log_backend.h>
#endif

#include <pqxx/pqxx>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace NTpcc {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t MAX_LOADER_THREADS = 100;

void SetSearchPath(pqxx::connection& conn, const std::string& path) {
    if (!path.empty()) {
        pqxx::nontransaction ntx(conn);
        ntx.exec(fmt::format("SET search_path TO {}", conn.quote_name(path)));
    }
}

void DisableSynchronousCommit(pqxx::connection& conn) {
    pqxx::nontransaction ntx(conn);
    ntx.exec("SET synchronous_commit = off");
}

constexpr size_t BYTES_PER_ITEM = 40;
constexpr size_t BYTES_PER_STOCK = 280;
constexpr size_t BYTES_PER_CUSTOMER = 600;
constexpr size_t BYTES_PER_HISTORY = 46;
constexpr size_t BYTES_PER_ORDER = 48;
constexpr size_t BYTES_PER_NEW_ORDER = 12;
constexpr size_t BYTES_PER_ORDER_LINE = 54;
constexpr size_t AVG_ORDER_LINES_PER_ORDER = 10;
constexpr size_t NEW_ORDERS_PER_DISTRICT = CUSTOMERS_PER_DISTRICT - FIRST_UNPROCESSED_O_ID + 1;

size_t EstimateSharedDataSize() {
    return ITEM_COUNT * BYTES_PER_ITEM;
}

size_t EstimatePerWarehouseDataSize() {
    size_t stock = ITEM_COUNT * BYTES_PER_STOCK;
    size_t perDistrict =
        CUSTOMERS_PER_DISTRICT * BYTES_PER_CUSTOMER +
        CUSTOMERS_PER_DISTRICT * BYTES_PER_HISTORY +
        CUSTOMERS_PER_DISTRICT * BYTES_PER_ORDER +
        NEW_ORDERS_PER_DISTRICT * BYTES_PER_NEW_ORDER +
        CUSTOMERS_PER_DISTRICT * AVG_ORDER_LINES_PER_ORDER * BYTES_PER_ORDER_LINE;
    // warehouse + districts + stock + per-district tables
    return sizeof(int) * 8 + DISTRICT_COUNT * 64 + stock + DISTRICT_COUNT * perDistrict;
}

void ThrowIfFailed(const TPutBatchResult& result, const char* what) {
    if (result.Outcome != EPutBatchOutcome::Completed) {
        throw std::runtime_error(fmt::format("{} failed: {}", what, result.Message));
    }
}

} // anonymous

void ImportSync(const TImportConfig& config) {
    std::vector<TWarehouseRange> ranges = config.WarehouseRanges;
    if (ranges.empty()) {
        if (config.WarehouseCount == 0) {
            LOG_E("Specified zero warehouses");
            throw std::runtime_error("Warehouse count must be greater than zero");
        }
        ranges.push_back(TWarehouseRange{1, static_cast<int>(config.WarehouseCount) + 1});
    }

    const size_t assignedWarehouses = CountWarehouses(ranges);
    const int scaleWarehouses = config.TotalWarehouses > 0
        ? config.TotalWarehouses
        : static_cast<int>(assignedWarehouses);

    size_t threadCount = config.LoadThreadCount;
    if (threadCount == 0) {
        threadCount = std::min({assignedWarehouses, NumberOfMyCpus(), MAX_LOADER_THREADS});
    }
    threadCount = std::max(threadCount, size_t(1));
    threadCount = std::min(threadCount, assignedWarehouses);

    LOG_I("Starting idempotent TPC-C import for {} assigned warehouses (scale {}) "
          "using {} threads (seed={}, run_id={}, batch_rows={})",
          assignedWarehouses, scaleWarehouses, threadCount, config.Seed,
          config.RunId.empty() ? "-" : config.RunId, config.BatchRows);

    auto startTime = Clock::now();

    TImportState state{GetGlobalInterruptSource().get_token()};
    state.ApproximateDataSize =
        (config.OwnsGlobalData ? EstimateSharedDataSize() : 0) +
        assignedWarehouses * EstimatePerWarehouseDataSize();

    const uint64_t seed = config.Seed;
    const std::string& runId = config.RunId;

    // Global item table (upsert). Safe to retry; does not disturb warehouse stock FKs.
    if (config.OwnsGlobalData) {
        pqxx::connection conn(config.ConnectionString);
        SetSearchPath(conn, config.Path);
        DisableSynchronousCommit(conn);
        ThrowIfFailed(
            PutItemsIdempotent(conn, seed, runId, config.BatchRows), "item PutBatch");
        state.DataSizeLoaded.fetch_add(EstimateSharedDataSize(), std::memory_order_relaxed);
    }

    std::vector<int> warehouseIds;
    for (const auto& range : ranges) {
        for (int wh = range.Start; wh < range.End; ++wh) {
            warehouseIds.push_back(wh);
        }
    }

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (size_t tid = 0; tid < threadCount; ++tid) {
        const size_t begin = tid * warehouseIds.size() / threadCount;
        const size_t end = (tid + 1) * warehouseIds.size() / threadCount;

        threads.emplace_back([&config, &state, &warehouseIds, begin, end, assignedWarehouses, seed,
                              &runId]() {
            try {
                pqxx::connection conn(config.ConnectionString);
                SetSearchPath(conn, config.Path);
                DisableSynchronousCommit(conn);
                for (size_t i = begin; i < end; ++i) {
                    if (state.StopToken.stop_requested()) {
                        return;
                    }
                    const int wh = warehouseIds[i];
                    const auto batchResult = PutWarehouseIdempotent(
                        conn, seed, wh, runId, config.BatchRows);
                    if (batchResult.Outcome != EPutBatchOutcome::Completed) {
                        throw std::runtime_error(fmt::format(
                            "warehouse {} PutBatch failed: {}", wh, batchResult.Message));
                    }

                    state.DataSizeLoaded.fetch_add(
                        EstimatePerWarehouseDataSize(), std::memory_order_relaxed);
                    state.WarehousesLoaded.fetch_add(1, std::memory_order_relaxed);

                    LOG_I("Warehouse {} replaced ({}/{})",
                          wh, state.WarehousesLoaded.load(), assignedWarehouses);
                }
            } catch (const std::exception& ex) {
                LOG_E("Import thread failed: {}", ex.what());
                RequestStopWithError();
            }
        });
    }

#ifdef TPCC_HAS_TUI
    TLogCapture logCapture(TUI_LOG_LINES);
    std::unique_ptr<TImportTui> tui;
    if (config.UseTui) {
        StartLogCapture(logCapture);
        TImportDisplayData initData(state);
        tui = std::make_unique<TImportTui>(
            logCapture, assignedWarehouses, threadCount, initData);
    }
#endif

    {
        size_t prevLoaded = state.DataSizeLoaded.load(std::memory_order_relaxed);
        auto prevTime = Clock::now();

        while (state.WarehousesLoaded.load(std::memory_order_relaxed) < assignedWarehouses
               && !state.StopToken.stop_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto now = Clock::now();
            auto elapsed = std::chrono::duration<double>(now - startTime);
            size_t loaded = state.DataSizeLoaded.load(std::memory_order_relaxed);

#ifdef TPCC_HAS_TUI
            if (tui) {
                TImportDisplayData data(state);
                auto& s = data.StatusData;
                s.CurrentDataSizeLoaded = loaded;
                s.PercentLoaded = state.ApproximateDataSize > 0
                    ? 100.0 * loaded / state.ApproximateDataSize : 0;

                auto sincePrev = std::chrono::duration<double>(now - prevTime);
                if (sincePrev.count() > 0.01) {
                    s.InstantSpeedMiBs =
                        (loaded - prevLoaded) / (1024.0 * 1024.0) / sincePrev.count();
                }
                if (elapsed.count() > 0.01) {
                    s.AvgSpeedMiBs = loaded / (1024.0 * 1024.0) / elapsed.count();
                }

                int totalSec = static_cast<int>(elapsed.count());
                s.ElapsedMinutes = totalSec / 60;
                s.ElapsedSeconds = totalSec % 60;

                if (s.AvgSpeedMiBs > 0.01 && state.ApproximateDataSize > loaded) {
                    double remainSec =
                        (state.ApproximateDataSize - loaded) / (s.AvgSpeedMiBs * 1024 * 1024);
                    int etaSec = static_cast<int>(remainSec);
                    s.EstimatedTimeLeftMinutes = etaSec / 60;
                    s.EstimatedTimeLeftSeconds = etaSec % 60;
                }

                tui->Update(data);
                prevLoaded = loaded;
                prevTime = now;
            }
#endif
        }
    }

    bool wasInterrupted = GetGlobalInterruptSource().stop_requested();

#ifdef TPCC_HAS_TUI
    tui.reset();
    StopLogCapture();
#endif

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (wasInterrupted) {
        throw std::runtime_error("Import was interrupted or failed. See logs.");
    }

    CreateIndexes(config.ConnectionString, config.Path);

    LOG_I("Running ANALYZE on TPC-C tables...");
    {
        pqxx::connection conn(config.ConnectionString);
        SetSearchPath(conn, config.Path);
        pqxx::nontransaction ntx(conn);
        for (const auto* table : TPCC_TABLES) {
            ntx.exec(fmt::format("ANALYZE {}", table));
        }
    }

    auto elapsed = std::chrono::duration<double>(Clock::now() - startTime);
    LOG_I("Import completed successfully in {:.1f}s", elapsed.count());
}

} // namespace NTpcc
