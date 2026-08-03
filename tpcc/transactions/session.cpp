#include "session.h"

#include <future.h>

namespace NTpcc {

TFuture<TOperationResult> ITpccTransaction::ExecuteSelect1() {
    TPromise<TOperationResult> promise;
    auto future = promise.GetFuture();
    TOperationResult result;
    result.Ok = false;
    result.ErrorClass = EErrorClass::Permanent;
    result.Message = "ExecuteSelect1 not supported";
    promise.SetValue(std::move(result));
    return future;
}

} // namespace NTpcc
