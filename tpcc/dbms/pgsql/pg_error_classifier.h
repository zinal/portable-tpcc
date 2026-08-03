#pragma once

#include <error_classifier.h>

#include <pqxx/pqxx>

#include <exception>
#include <string>

namespace NTpcc {

class TPgErrorClassifier : public IErrorClassifier {
public:
    EErrorClass Classify(
        std::string_view nativeCode,
        std::string_view message = {}) const override;

    // Classify a caught libpqxx exception (SQLSTATE when available).
    EErrorClass ClassifyException(const std::exception& ex) const override;

    // When a failure happens around Commit(), connection loss is AmbiguousCommit.
    EErrorClass ClassifyCommitException(const std::exception& ex) const;
};

std::string PgSqlStateOf(const std::exception& ex);

} // namespace NTpcc
