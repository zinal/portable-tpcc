#include "import.h"

#include "init.h"
#include "load_batch.h"
#include "ob_connection.h"

#include <constants.h>
#include <domain_util.h>
#include <log.h>
#include <warehouse_range.h>

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
    return sizeof(int) * 8 + DISTRICT_COUNT * 64 + stock + DISTRICT_COUNT * perDistrict;
}

void ThrowIfFailed(const TPutBatchResult& result, const char* what) {
    if (result.Outcome != EPutBatchOutcome::Completed) {
        throw std::runtime_error(fmt::format("{} failed: {}", what, result.Message));
    }
}

int CountItems(TObConnection& conn) {
    auto result = conn.QuerySimple("SELECT COUNT(*) AS cnt FROM item");
    if (!result.TryNextRow()) {
        return 0;
    }
    return result.GetInt32("cnt");
}

// Non-owners must not insert stock (FK to item) until the global owner finishes items.
void WaitForGlobalItems(TObConnection& conn, std::stop_token stop) {
    LOG_I("Waiting for global item table (" << ITEM_COUNT
          << " rows) before loading assigned warehouses");
    auto lastLog = Clock::now();
    while (!stop.stop_requested()) {
        const int count = CountItems(conn);
        if (count >= ITEM_COUNT) {
            LOG_I("Global item table ready (" << count << " rows)");
            return;
        }
        if (Clock::now() - lastLog >= std::chrono::seconds(15)) {
            LOG_I("Still waiting for item table (" << count << "/" << ITEM_COUNT << ")");
            lastLog = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    throw std::runtime_error("Interrupted while waiting for global item table");
}

} // namespace

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

    LOG_I("Starting idempotent TPC-C import for " << assignedWarehouses
          << " assigned warehouses (scale " << scaleWarehouses
          << ", ranges=" << FormatWarehouseRanges(ranges)
          << ", owns_global_data=" << (config.OwnsGlobalData ? "true" : "false")
          << ") using " << threadCount << " threads (seed=" << config.Seed
          << ", run_id=" << (config.RunId.empty() ? "-" : config.RunId)
          << ", batch_rows=" << config.BatchRows << ")");

    auto startTime = Clock::now();

    TImportState state{GetGlobalInterruptSource().get_token()};
    state.ApproximateDataSize =
        (config.OwnsGlobalData ? EstimateSharedDataSize() : 0) +
        assignedWarehouses * EstimatePerWarehouseDataSize();

    const uint64_t seed = config.Seed;
    const std::string& runId = config.RunId;

    if (config.OwnsGlobalData) {
        auto conn = ConnectToTargetDatabase(ConfigWithPath(config.ConnectionString, config.Path));
        conn->ConfigureBulkLoadSession();
        ThrowIfFailed(PutItemsIdempotent(*conn, seed, runId, config.BatchRows), "item PutBatch");
        state.DataSizeLoaded.fetch_add(EstimateSharedDataSize(), std::memory_order_relaxed);
    } else {
        auto conn = ConnectToTargetDatabase(ConfigWithPath(config.ConnectionString, config.Path));
        WaitForGlobalItems(*conn, state.StopToken);
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
                auto conn = ConnectToTargetDatabase(ConfigWithPath(config.ConnectionString, config.Path));
                conn->ConfigureBulkLoadSession();
                for (size_t i = begin; i < end; ++i) {
                    if (state.StopToken.stop_requested()) {
                        return;
                    }
                    const int wh = warehouseIds[i];
                    LOG_I("Loading warehouse " << wh);
                    const auto batchResult = PutWarehouseIdempotent(
                        *conn, seed, wh, runId, config.BatchRows);
                    if (batchResult.Outcome != EPutBatchOutcome::Completed) {
                        throw std::runtime_error(fmt::format(
                            "warehouse {} PutBatch failed: {}", wh, batchResult.Message));
                    }

                    state.DataSizeLoaded.fetch_add(
                        EstimatePerWarehouseDataSize(), std::memory_order_relaxed);
                    state.WarehousesLoaded.fetch_add(1, std::memory_order_relaxed);

                    LOG_I("Warehouse " << wh << " loaded (" << state.WarehousesLoaded.load()
                          << "/" << assignedWarehouses << ")");
                }
            } catch (const std::exception& ex) {
                LOG_E("Import thread failed: " << ex.what());
                RequestStopWithError();
            }
        });
    }

    auto lastProgress = Clock::now();
    while (state.WarehousesLoaded.load(std::memory_order_relaxed) < assignedWarehouses
           && !state.StopToken.stop_requested())
    {
        if (Clock::now() - lastProgress >= std::chrono::seconds(15)) {
            LOG_I("Import progress: " << state.WarehousesLoaded.load()
                  << "/" << assignedWarehouses
                  << " warehouses (first warehouse includes ~100k stock rows)");
            lastProgress = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    bool wasInterrupted = GetGlobalInterruptSource().stop_requested();

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (wasInterrupted) {
        throw std::runtime_error("Import was interrupted or failed. See logs.");
    }

    // Indexes/ANALYZE are DB-wide; only the global-data owner runs them so
    // earlier-finishing shard loaders do not lock tables still being loaded.
    if (config.OwnsGlobalData) {
        CreateIndexes(config.ConnectionString, config.Path);
        AnalyzeTables(config.ConnectionString, config.Path);
    } else {
        LOG_I("Skipping CreateIndexes/ANALYZE (owned by global-data loader)");
    }

    auto elapsed = std::chrono::duration<double>(Clock::now() - startTime);
    LOG_I(fmt::format("Import completed successfully in {:.1f}s", elapsed.count()));
}

} // namespace NTpcc
