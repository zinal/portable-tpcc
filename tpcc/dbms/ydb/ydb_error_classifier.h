#pragma once

#include <error_classifier.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>

#include <exception>
#include <string>
#include <string_view>

namespace NTpcc {

class TYdbErrorClassifier : public IErrorClassifier {
public:
    EErrorClass Classify(std::string_view nativeCode, std::string_view message = {}) const override;
    EErrorClass ClassifyStatus(const NYdb::TStatus& status, bool duringCommit = false) const;
    EErrorClass ClassifyException(const std::exception& ex, bool duringCommit = false) const;
};

std::string YdbStatusCodeOf(NYdb::EStatus status);
std::string YdbIssuesToString(const NYdb::TStatus& status);

} // namespace NTpcc
