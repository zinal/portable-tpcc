#include "workflows.h"
#include "workflow_util.h"

#include <constants.h>
#include <log.h>
#include <money.h>
#include <rng.h>

#include <string>

namespace NTpcc {

TFuture<bool> GetPaymentTask(
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
        TMoney PaymentAmount;
        int CustomerDistrictID;
        int CustomerWarehouseID;
        bool LookupByName;
        std::string LastName;
        int CustomerID;
    };

    const auto& in = FixedTransactionInputs<TInputs>(context, [&] {
        TInputs generated;
        generated.WarehouseID = static_cast<int>(context.WarehouseID);
        generated.DistrictID = RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID);
        generated.PaymentAmount = TMoney::FromCents(RandomNumber(100, 500000));

        if (RandomNumber(1, 100) <= 85) {
            generated.CustomerDistrictID = generated.DistrictID;
            generated.CustomerWarehouseID = generated.WarehouseID;
        } else {
            generated.CustomerDistrictID = RandomNumber(DISTRICT_LOW_ID, DISTRICT_HIGH_ID);
            do {
                generated.CustomerWarehouseID = RandomNumber(1, context.WarehouseCount);
            } while (generated.CustomerWarehouseID == generated.WarehouseID &&
                     context.WarehouseCount > 1);
        }

        generated.LookupByName = RandomNumber(1, 100) <= 60;
        if (generated.LookupByName) {
            generated.LastName = GetNonUniformRandomLastNameForRun();
            generated.CustomerID = 0;
        } else {
            generated.CustomerID = GetRandomCustomerID();
        }
        return generated;
    });

    LOG_T("Terminal {} started Payment: W={}, D={}",
          context.TerminalID, in.WarehouseID, in.DistrictID);

    TWarehouseDistrictInfo loc;
    {
        auto r = co_await SuspendExecute(tx, context, TApplyPaymentToLocation{
            in.WarehouseID, in.DistrictID, in.PaymentAmount});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "Payment location update failed");
        }
        loc = std::get<TWarehouseDistrictInfo>(r.Payload);
    }

    TCustomerRow customer;
    if (in.LookupByName) {
        auto r = co_await SuspendExecute(tx, context, TGetCustomersByLastName{
            in.CustomerWarehouseID, in.CustomerDistrictID, in.LastName});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "Payment customer by name failed");
        }
        auto selected = SelectCustomerByLastNameMedian(
            std::get<std::vector<TCustomerRow>>(r.Payload));
        if (!selected) {
            co_return FailPermanent(context.TerminalID, "Payment no customer by name");
        }
        customer = std::move(*selected);
    } else {
        auto r = co_await SuspendExecute(tx, context, TGetCustomerById{
            in.CustomerWarehouseID, in.CustomerDistrictID, in.CustomerID});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "Payment customer not found");
        }
        customer = std::get<TCustomerRow>(r.Payload);
    }

    customer.Balance -= in.PaymentAmount;
    customer.YtdPayment += in.PaymentAmount;
    customer.PaymentCount += 1;

    if (customer.Credit == "BC") {
        std::string cData;
        {
            auto r = co_await SuspendExecute(tx, context, TGetCustomerData{
                in.CustomerWarehouseID, in.CustomerDistrictID, customer.CustomerID});
            ThrowIfRetryable(r);
            if (r.Ok) {
                cData = std::get<std::string>(r.Payload);
            }
        }

        std::string newData =
            std::to_string(customer.CustomerID) + " " +
            std::to_string(in.CustomerDistrictID) + " " +
            std::to_string(in.CustomerWarehouseID) + " " +
            std::to_string(in.DistrictID) + " " +
            std::to_string(in.WarehouseID) + " " +
            in.PaymentAmount.ToString() + " | " + cData;
        if (newData.size() > 500) {
            newData.resize(500);
        }

        auto r = co_await SuspendExecute(tx, context, TUpdateCustomerPayment{
            in.CustomerWarehouseID, in.CustomerDistrictID, customer.CustomerID,
            customer.Balance, customer.YtdPayment, customer.PaymentCount,
            true, std::move(newData)});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "Payment update customer failed");
        }
    } else {
        auto r = co_await SuspendExecute(tx, context, TUpdateCustomerPayment{
            in.CustomerWarehouseID, in.CustomerDistrictID, customer.CustomerID,
            customer.Balance, customer.YtdPayment, customer.PaymentCount,
            false, {}});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "Payment update customer failed");
        }
    }

    std::string historyData = loc.WarehouseName + "    " + loc.DistrictName;
    if (historyData.size() > 24) {
        historyData.resize(24);
    }

    {
        auto r = co_await SuspendExecute(tx, context, TInsertPaymentHistory{
            in.CustomerWarehouseID, in.CustomerDistrictID, customer.CustomerID,
            in.WarehouseID, in.DistrictID, in.PaymentAmount, historyData});
        ThrowIfRetryable(r);
        if (!r.Ok) {
            co_return FailPermanent(context.TerminalID, "Payment insert history failed");
        }
    }

    LOG_T("Terminal {} committing Payment", context.TerminalID);
    auto commit = co_await SuspendCommit(tx, context);
    ThrowIfCommitFailed(commit);

    latency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTs);
    co_return true;
}

} // namespace NTpcc
