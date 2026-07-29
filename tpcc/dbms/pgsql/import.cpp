#include "import.h"
#include "warehouse_range.h"
#include "pqxx_compat.h"

#include <constants.h>
#include "init.h"
#include <log.h>
#include <domain_util.h>
#include <populate.h>

#ifdef TPCC_HAS_TUI
#include "import_tui.h"
#include <log_backend.h>
#endif

#include <pqxx/pqxx>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
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

// Rough per-row byte estimates for progress tracking (not exact, but close enough for TUI)
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
    return stock + DISTRICT_COUNT * perDistrict;
}

//-----------------------------------------------------------------------------

std::string FormatUnixTimestamp(int64_t unixSeconds) {
    time_t t = static_cast<time_t>(unixSeconds);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void LoadItems(pqxx::connection& conn, uint64_t seed) {
    LOG_I("Loading {} items (seed={})...", ITEM_COUNT, seed);

    pqxx::work txn(conn);
    auto stream = MakeCopyStream(txn, "item",
        {"i_id", "i_name", "i_price", "i_data", "i_im_id"});

    for (int i = 1; i <= ITEM_COUNT; ++i) {
        auto row = NGenerator::GenerateItem(seed, i);
        stream.write_values(
            row.Id,
            row.Name,
            row.Price.ToString(),
            row.Data,
            row.ImageId
        );
    }

    stream.complete();
    txn.commit();
    LOG_I("Items loaded");
}

void LoadWarehouses(pqxx::connection& conn, uint64_t seed, int startId, int lastId) {
    LOG_I("Loading warehouses {} to {}", startId, lastId);

    pqxx::work txn(conn);
    auto stream = MakeCopyStream(txn, "warehouse",
        {"w_id", "w_ytd", "w_tax", "w_name", "w_street_1", "w_street_2",
         "w_city", "w_state", "w_zip"});

    for (int wh = startId; wh <= lastId; ++wh) {
        auto row = NGenerator::GenerateWarehouse(seed, wh);
        stream.write_values(
            row.Id,
            row.Ytd.ToString(),
            row.Tax.ToString(),
            row.Name,
            row.Street1,
            row.Street2,
            row.City,
            row.State,
            row.Zip
        );
    }

    stream.complete();
    txn.commit();
}

void LoadDistricts(pqxx::connection& conn, uint64_t seed, int startId, int lastId) {
    LOG_I("Loading districts for warehouses {} to {}", startId, lastId);

    pqxx::work txn(conn);
    auto stream = MakeCopyStream(txn, "district",
        {"d_w_id", "d_id", "d_ytd", "d_tax", "d_next_o_id", "d_name",
         "d_street_1", "d_street_2", "d_city", "d_state", "d_zip"});

    for (int wh = startId; wh <= lastId; ++wh) {
        for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
            auto row = NGenerator::GenerateDistrict(seed, wh, d);
            stream.write_values(
                row.WarehouseId,
                row.Id,
                row.Ytd.ToString(),
                row.Tax.ToString(),
                row.NextOrderId,
                row.Name,
                row.Street1,
                row.Street2,
                row.City,
                row.State,
                row.Zip
            );
        }
    }

    stream.complete();
    txn.commit();
}

void LoadStock(pqxx::connection& conn, uint64_t seed, int wh) {
    LOG_D("Loading stock for warehouse {}", wh);

    pqxx::work txn(conn);
    auto stream = MakeCopyStream(txn, "stock",
        {"s_w_id", "s_i_id", "s_quantity", "s_ytd", "s_order_cnt", "s_remote_cnt",
         "s_data", "s_dist_01", "s_dist_02", "s_dist_03", "s_dist_04", "s_dist_05",
         "s_dist_06", "s_dist_07", "s_dist_08", "s_dist_09", "s_dist_10"});

    for (int itemId = 1; itemId <= ITEM_COUNT; ++itemId) {
        auto row = NGenerator::GenerateStock(seed, wh, itemId);
        stream.write_values(
            row.WarehouseId,
            row.ItemId,
            row.Quantity,
            row.Ytd.ToString(),
            row.OrderCount,
            row.RemoteCount,
            row.Data,
            row.Dist[0],
            row.Dist[1],
            row.Dist[2],
            row.Dist[3],
            row.Dist[4],
            row.Dist[5],
            row.Dist[6],
            row.Dist[7],
            row.Dist[8],
            row.Dist[9]
        );
    }

    stream.complete();
    txn.commit();
}

void LoadCustomers(pqxx::connection& conn, uint64_t seed, int wh, int district) {
    LOG_D("Loading customers for warehouse {} district {}", wh, district);

    pqxx::work txn(conn);
    auto stream = MakeCopyStream(txn, "customer",
        {"c_w_id", "c_d_id", "c_id", "c_discount", "c_credit", "c_last", "c_first",
         "c_credit_lim", "c_balance", "c_ytd_payment", "c_payment_cnt", "c_delivery_cnt",
         "c_street_1", "c_street_2", "c_city", "c_state", "c_zip", "c_phone",
         "c_since", "c_middle", "c_data"});

    for (int cid = C_FIRST_CUSTOMER_ID; cid <= CUSTOMERS_PER_DISTRICT; ++cid) {
        auto row = NGenerator::GenerateCustomer(seed, wh, district, cid);
        stream.write_values(
            row.WarehouseId,
            row.DistrictId,
            row.Id,
            row.Discount.ToString(),
            row.Credit,
            row.Last,
            row.First,
            row.CreditLimit.ToString(),
            row.Balance.ToString(),
            row.YtdPayment.ToString(),
            row.PaymentCount,
            row.DeliveryCount,
            row.Street1,
            row.Street2,
            row.City,
            row.State,
            row.Zip,
            row.Phone,
            FormatUnixTimestamp(row.SinceUnix),
            row.Middle,
            row.Data
        );
    }

    stream.complete();
    txn.commit();
}

void LoadHistory(pqxx::connection& conn, uint64_t seed, int wh, int district) {
    LOG_D("Loading history for warehouse {} district {}", wh, district);

    pqxx::work txn(conn);
    auto stream = MakeCopyStream(txn, "history",
        {"h_c_id", "h_c_d_id", "h_c_w_id", "h_d_id", "h_w_id", "h_date", "h_amount", "h_data"});

    for (int cid = C_FIRST_CUSTOMER_ID; cid <= CUSTOMERS_PER_DISTRICT; ++cid) {
        auto row = NGenerator::GenerateHistory(seed, wh, district, cid);
        stream.write_values(
            row.CustomerId,
            row.CustomerDistrictId,
            row.CustomerWarehouseId,
            row.DistrictId,
            row.WarehouseId,
            FormatUnixTimestamp(row.DateUnix),
            row.Amount.ToString(),
            row.Data
        );
    }

    stream.complete();
    txn.commit();
}

void LoadOrders(pqxx::connection& conn, uint64_t seed, int wh, int district) {
    LOG_D("Loading orders for warehouse {} district {}", wh, district);

    const auto customerIds = NGenerator::InitialOrderCustomerPermutation(seed, wh, district);

    {
        pqxx::work txn(conn);
        auto stream = MakeCopyStream(txn, "oorder",
            {"o_w_id", "o_d_id", "o_id", "o_c_id", "o_carrier_id", "o_ol_cnt",
             "o_all_local", "o_entry_d"});

        for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
            int cid = customerIds[oid - 1];
            auto row = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
            stream.write_values(
                row.WarehouseId,
                row.DistrictId,
                row.Id,
                row.CustomerId,
                row.CarrierId,
                row.OlCnt,
                row.AllLocal,
                FormatUnixTimestamp(row.EntryUnix)
            );
        }

        stream.complete();
        txn.commit();
    }

    {
        pqxx::work txn(conn);
        auto stream = MakeCopyStream(txn, "new_order",
            {"no_w_id", "no_d_id", "no_o_id"});

        for (int oid = FIRST_UNPROCESSED_O_ID; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
            auto row = NGenerator::GenerateNewOrder(wh, district, oid);
            stream.write_values(row.WarehouseId, row.DistrictId, row.OrderId);
        }

        stream.complete();
        txn.commit();
    }

    {
        pqxx::work txn(conn);
        auto stream = MakeCopyStream(txn, "order_line",
            {"ol_w_id", "ol_d_id", "ol_o_id", "ol_number", "ol_i_id", "ol_delivery_d",
             "ol_amount", "ol_supply_w_id", "ol_quantity", "ol_dist_info"});

        for (int oid = 1; oid <= CUSTOMERS_PER_DISTRICT; ++oid) {
            int cid = customerIds[oid - 1];
            auto order = NGenerator::GenerateOrder(seed, wh, district, oid, cid);
            const bool delivered = oid < FIRST_UNPROCESSED_O_ID;
            auto lines = NGenerator::GenerateOrderLines(
                seed, wh, district, oid, order.OlCnt, delivered);
            for (const auto& line : lines) {
                std::optional<std::string> deliveryDate;
                if (line.DeliveryUnix) {
                    deliveryDate = FormatUnixTimestamp(*line.DeliveryUnix);
                }
                stream.write_values(
                    line.WarehouseId,
                    line.DistrictId,
                    line.OrderId,
                    line.Number,
                    line.ItemId,
                    deliveryDate,
                    line.Amount.ToString(),
                    line.SupplyWarehouseId,
                    line.Quantity,
                    line.DistInfo
                );
            }
        }

        stream.complete();
        txn.commit();
    }
}

void LoadWarehouse(pqxx::connection& conn, uint64_t seed, int wh, TImportState& state) {
    if (state.StopToken.stop_requested()) return;

    LoadStock(conn, seed, wh);
    state.DataSizeLoaded.fetch_add(
        ITEM_COUNT * BYTES_PER_STOCK, std::memory_order_relaxed);

    for (int d = DISTRICT_LOW_ID; d <= DISTRICT_HIGH_ID; ++d) {
        if (state.StopToken.stop_requested()) return;

        LoadCustomers(conn, seed, wh, d);
        LoadHistory(conn, seed, wh, d);
        LoadOrders(conn, seed, wh, d);

        size_t districtBytes =
            CUSTOMERS_PER_DISTRICT * BYTES_PER_CUSTOMER +
            CUSTOMERS_PER_DISTRICT * BYTES_PER_HISTORY +
            CUSTOMERS_PER_DISTRICT * BYTES_PER_ORDER +
            NEW_ORDERS_PER_DISTRICT * BYTES_PER_NEW_ORDER +
            CUSTOMERS_PER_DISTRICT * AVG_ORDER_LINES_PER_ORDER * BYTES_PER_ORDER_LINE;
        state.DataSizeLoaded.fetch_add(districtBytes, std::memory_order_relaxed);
    }

    state.WarehousesLoaded.fetch_add(1, std::memory_order_relaxed);
}

} // anonymous

//-----------------------------------------------------------------------------

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

    LOG_I("Starting TPC-C data import for {} assigned warehouses (scale {}) using {} threads (seed={})",
          assignedWarehouses, scaleWarehouses, threadCount, config.Seed);

    auto startTime = Clock::now();

    TImportState state{GetGlobalInterruptSource().get_token()};
    state.ApproximateDataSize =
        (config.OwnsGlobalData ? EstimateSharedDataSize() : 0) +
        assignedWarehouses * EstimatePerWarehouseDataSize();

    const uint64_t seed = config.Seed;

    // Global tables and warehouse metadata for assigned ranges.
    {
        pqxx::connection conn(config.ConnectionString);
        SetSearchPath(conn, config.Path);
        DisableSynchronousCommit(conn);
        if (config.OwnsGlobalData) {
            LoadItems(conn, seed);
            state.DataSizeLoaded.fetch_add(EstimateSharedDataSize(), std::memory_order_relaxed);
        }
        for (const auto& range : ranges) {
            const int start = RangeStartInclusive(range);
            const int end = RangeEndInclusive(range);
            LoadWarehouses(conn, seed, start, end);
            LoadDistricts(conn, seed, start, end);
        }
    }

    // Flatten assigned warehouse ids for parallel per-warehouse loading.
    std::vector<int> warehouseIds;
    for (const auto& range : ranges) {
        for (int wh = range.Start; wh < range.End; ++wh) {
            warehouseIds.push_back(wh);
        }
    }

    // Load per-warehouse data in parallel
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (size_t tid = 0; tid < threadCount; ++tid) {
        const size_t begin = tid * warehouseIds.size() / threadCount;
        const size_t end = (tid + 1) * warehouseIds.size() / threadCount;

        threads.emplace_back([&config, &state, &warehouseIds, begin, end, assignedWarehouses, seed]() {
            try {
                pqxx::connection conn(config.ConnectionString);
                SetSearchPath(conn, config.Path);
                DisableSynchronousCommit(conn);
                for (size_t i = begin; i < end; ++i) {
                    if (state.StopToken.stop_requested()) return;
                    const int wh = warehouseIds[i];
                    LoadWarehouse(conn, seed, wh, state);

                    LOG_I("Warehouse {} loaded ({}/{})",
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
        if (t.joinable()) t.join();
    }

    if (wasInterrupted) {
        throw std::runtime_error("Import was interrupted or failed. See logs.");
    }

    LOG_I("Running ANALYZE on TPC-C tables...");
    {
        pqxx::connection conn(config.ConnectionString);
        SetSearchPath(conn, config.Path);
        pqxx::nontransaction ntx(conn);
        for (const auto* table : TPCC_TABLES) {
            ntx.exec(fmt::format("ANALYZE {}", table));
        }
    }

    CreateIndexes(config.ConnectionString, config.Path);

    auto elapsed = Clock::now() - startTime;
    auto seconds = std::chrono::duration<double>(elapsed).count();
    LOG_I("TPC-C data import completed successfully in {:.1f}s", seconds);
}

} // namespace NTpcc
