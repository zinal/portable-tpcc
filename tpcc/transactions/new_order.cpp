#include "workflows.h"
#include "workflow_util.h"

#include <constants.h>
#include <log.h>
#include <money.h>
#include <rng.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace NTpcc {

namespace {

struct TPairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& pair) const {
        return std::hash<T1>()(pair.first) ^ std::hash<T2>()(pair.second);
    }
};

} // namespace

TFuture<bool> GetNewOrderTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx)
{
    auto startTs = std::chrono::steady_clock::now();

    TTransactionInflightGuard guard;
    co_await TTaskReady(context.TaskQueue, context.TerminalID);

    struct TInputs {
        int WarehouseID;
        int DistrictID;
        int CustomerID;
        int NumItems;
        std::vector<int> ItemIDs;
        std::vector<int> SupplierWarehouseIDs;
        std::vector<int> OrderQuantities;
        int AllLocal;
    };

    const auto& in = FixedTransactionInputs<TInputs>(context, [&] {
        TInputs generated;
        generated.WarehouseID = static_cast<int>(context.WarehouseID);
        generated.DistrictID = RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID);
        generated.CustomerID = GetRandomCustomerID();
        generated.NumItems = RandomNumber(MIN_ITEMS, MAX_ITEMS);
        generated.ItemIDs.reserve(generated.NumItems);
        generated.SupplierWarehouseIDs.reserve(generated.NumItems);
        generated.OrderQuantities.reserve(generated.NumItems);
        generated.AllLocal = 1;

        for (int i = 0; i < generated.NumItems; ++i) {
            generated.ItemIDs.push_back(GetRandomItemID());
            if (context.WarehouseCount == 1 || RandomNumber(1, 100) > 1) {
                generated.SupplierWarehouseIDs.push_back(generated.WarehouseID);
            } else {
                int supplierID;
                do {
                    supplierID = RandomNumber(1, context.WarehouseCount);
                } while (supplierID == generated.WarehouseID);
                generated.SupplierWarehouseIDs.push_back(supplierID);
                generated.AllLocal = 0;
            }
            generated.OrderQuantities.push_back(RandomNumber(1, 10));
        }

        // TPC-C §2.4.1.5: 1% of New-Order inputs use an unused item number
        // on the last order line, producing the rollback profile in §2.4.2.3.
        if (RandomNumber(1, 100) == 1) {
            generated.ItemIDs[generated.NumItems - 1] = INVALID_ITEM_ID;
        }
        return generated;
    });

    LOG_T("Terminal " << context.TerminalID << " started NewOrder: W=" << in.WarehouseID << ", D=" << in.DistrictID << ", C=" << in.CustomerID);

    {
        auto r = co_await SuspendExecute(tx, context, TGetCustomerById{
            in.WarehouseID, in.DistrictID, in.CustomerID});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "NewOrder customer not found");
        }
    }

    {
        auto r = co_await SuspendExecute(tx, context, TGetWarehouseTax{in.WarehouseID});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "NewOrder warehouse not found");
        }
    }

    int nextOrderID = 0;
    {
        auto r = co_await SuspendExecute(tx, context, TReserveDistrictOrderId{
            in.WarehouseID, in.DistrictID});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "NewOrder district not found");
        }
        nextOrderID = std::get<TDistrictOrderReservation>(r.Payload).NextOrderID;
    }

    {
        auto r = co_await SuspendExecute(tx, context, TCreateOrder{
            in.WarehouseID, in.DistrictID, nextOrderID, in.CustomerID,
            in.NumItems, in.AllLocal});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "NewOrder create order failed");
        }
    }

    // TPC-C §2.4.2.3: for the 1% unused-item rollback profile, every valid
    // order line must still execute ITEM/STOCK/ORDER-LINE work. Knowledge of
    // the unused item may only skip STOCK/ORDER-LINE steps for that item
    // itself; the ITEM select that produces "not-found" is still required.
    std::vector<int> validItemIds;
    validItemIds.reserve(in.NumItems);
    for (int id : in.ItemIDs) {
        if (id != INVALID_ITEM_ID) {
            validItemIds.push_back(id);
        }
    }

    std::unordered_map<int, TMoney> itemPrices;
    if (!validItemIds.empty()) {
        auto r = co_await SuspendExecute(tx, context, TGetItems{validItemIds});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "NewOrder item not found");
        }
        for (const auto& item : std::get<std::vector<TItemRow>>(r.Payload)) {
            itemPrices[item.ItemID] = item.Price;
        }
    }

    std::vector<TStockKey> stockKeys;
    std::unordered_map<std::pair<int, int>, bool, TPairHash> seen;
    for (int i = 0; i < in.NumItems; ++i) {
        if (in.ItemIDs[i] == INVALID_ITEM_ID) {
            continue;
        }
        auto key = std::make_pair(in.SupplierWarehouseIDs[i], in.ItemIDs[i]);
        if (seen.count(key)) {
            continue;
        }
        seen[key] = true;
        stockKeys.push_back(TStockKey{in.SupplierWarehouseIDs[i], in.ItemIDs[i]});
    }

    std::unordered_map<std::pair<int, int>, TStockRow, TPairHash> stocks;
    if (!stockKeys.empty()) {
        auto r = co_await SuspendExecute(tx, context, TGetStocksForUpdate{in.DistrictID, stockKeys});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "NewOrder stock not found");
        }
        for (auto& row : std::get<std::vector<TStockRow>>(r.Payload)) {
            stocks[{row.WarehouseID, row.ItemID}] = std::move(row);
        }
    }

    for (int olNum = 1; olNum <= in.NumItems; ++olNum) {
        const int idx = olNum - 1;
        const int supWh = in.SupplierWarehouseIDs[idx];
        const int iid = in.ItemIDs[idx];
        const int qty = in.OrderQuantities[idx];

        if (iid == INVALID_ITEM_ID) {
            // TPC-C §2.4.2.3 / §5.1.2: only the expected ITEM not-found followed
            // by a confirmed rollback may be counted as intentional UserAborted.
            auto r = co_await SuspendExecute(tx, context, TGetItems{{iid}});
            ThrowIfRetryable(r);
            if (r.Ok) {
                co_return FailPermanent(context.TerminalID,
                    "NewOrder expected unused item to be missing");
            }
            if (!IsExpectedItemNotFound(r)) {
                throw TClassifiedError(
                    r.ErrorClass,
                    r.NativeCode,
                    r.Message.empty() ? "NewOrder unused item lookup failed" : r.Message);
            }
            auto rollback = co_await SuspendRollback(tx, context);
            ThrowIfRollbackFailed(rollback);
            throw TUserAbortedException();
        }

        auto priceIt = itemPrices.find(iid);
        if (priceIt == itemPrices.end()) {
            co_return FailPermanent(context.TerminalID, "NewOrder item price missing");
        }
        const TMoney olAmount = TMoney::FromCents(
            static_cast<int64_t>(qty) * priceIt->second.Cents());

        auto stockIt = stocks.find({supWh, iid});
        if (stockIt == stocks.end()) {
            co_return FailPermanent(context.TerminalID, "NewOrder stock missing");
        }
        auto& stock = stockIt->second;
        if (stock.Quantity - qty >= 10) {
            stock.Quantity -= qty;
        } else {
            stock.Quantity += -qty + 91;
        }

        {
            auto r = co_await SuspendExecute(tx, context, TUpdateStock{
                supWh, iid, stock.Quantity, qty,
                (supWh == in.WarehouseID ? 0 : 1)});
            ThrowIfRetryable(r);
            if (!r.Ok) {
                co_return FailPermanent(context.TerminalID, "NewOrder update stock failed");
            }
        }

        {
            auto r = co_await SuspendExecute(tx, context, TInsertOrderLine{
                in.WarehouseID, in.DistrictID, nextOrderID, olNum, iid,
                supWh, qty, olAmount, stock.DistInfo});
            ThrowIfRetryable(r);
            if (!r.Ok) {
                co_return FailPermanent(context.TerminalID, "NewOrder insert order line failed");
            }
        }
    }

    LOG_T("Terminal " << context.TerminalID << " committing NewOrder");
    auto commit = co_await SuspendCommit(tx, context);
    ThrowIfCommitFailed(commit);

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc
