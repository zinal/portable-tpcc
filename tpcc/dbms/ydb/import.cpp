#include "import.h"

#include "init.h"
#include "load_batch.h"

#include <constants.h>
#include <domain_util.h>
#include <log.h>
#include <warehouse_range.h>

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
constexpr size_t BYTES_PER_HISTORY = 64;
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
        throw std::runtime_error(std::string(what) + " failed: " + result.Message);
    }
}

} // anonymous

void ImportSync(const TImportConfig& config) {
    std::vector<TWarehouseRange> ranges = config.WarehouseRanges;
    if (ranges.empty()) {
        if (config.WarehouseCount == 0) {
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

    LOG_I("Starting YDB idempotent TPC-C import (BulkUpsert) for " << assignedWarehouses
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

    if (config.OwnsGlobalData) {
        TYdbConnection connection(config.Connection);
        ThrowIfFailed(PutItemsIdempotent(connection, config.Seed, config.RunId, config.BatchRows), "item PutBatch");
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
        threads.emplace_back([&config, &state, &warehouseIds, begin, end, assignedWarehouses]() {
            try {
                TYdbConnection connection(config.Connection);
                for (size_t i = begin; i < end; ++i) {
                    if (state.StopToken.stop_requested()) {
                        return;
                    }
                    const int wh = warehouseIds[i];
                    LOG_I("Loading warehouse " << wh);
                    auto result = PutWarehouseIdempotent(
                        connection, config.Seed, wh, config.RunId, config.BatchRows);
                    if (result.Outcome != EPutBatchOutcome::Completed) {
                        throw std::runtime_error("warehouse PutBatch failed: " + result.Message);
                    }
                    state.DataSizeLoaded.fetch_add(EstimatePerWarehouseDataSize(), std::memory_order_relaxed);
                    state.WarehousesLoaded.fetch_add(1, std::memory_order_relaxed);
                    LOG_I("Warehouse " << wh << " loaded (" << state.WarehousesLoaded.load()
                          << "/" << assignedWarehouses << ")");
                }
            } catch (const std::exception& ex) {
                LOG_E("YDB import thread failed: " << ex.what());
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

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (GetGlobalInterruptSource().stop_requested()) {
        throw std::runtime_error("YDB import interrupted or failed");
    }

    if (config.OwnsGlobalData) {
        CreateIndexes(config.Connection);
    } else {
        LOG_I("Skipping CreateIndexes (owned by global-data loader)");
    }

    auto elapsed = std::chrono::duration<double>(Clock::now() - startTime).count();
    LOG_I("YDB import completed: " << assignedWarehouses << " warehouses in " << elapsed << " seconds");
}

} // namespace NTpcc
