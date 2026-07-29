#include "pg_error_classifier.h"

#include <cctype>

namespace NTpcc {

namespace {

bool ContainsInsensitive(std::string_view hay, std::string_view needle) {
    if (needle.empty() || hay.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string PgSqlStateOf(const std::exception& ex) {
    if (const auto* sql = dynamic_cast<const pqxx::sql_error*>(&ex)) {
        return sql->sqlstate();
    }
    return {};
}

EErrorClass TPgErrorClassifier::Classify(
    std::string_view nativeCode,
    std::string_view message) const
{
    if (!nativeCode.empty()) {
        return ClassifySqlState(nativeCode);
    }
    if (ContainsInsensitive(message, "serialization") ||
        ContainsInsensitive(message, "deadlock") ||
        ContainsInsensitive(message, "could not serialize"))
    {
        return EErrorClass::RetryableAbort;
    }
    return EErrorClass::Permanent;
}

EErrorClass TPgErrorClassifier::ClassifyException(const std::exception& ex) const {
    if (dynamic_cast<const pqxx::transaction_rollback*>(&ex) != nullptr) {
        const auto state = PgSqlStateOf(ex);
        if (!state.empty()) {
            return ClassifySqlState(state);
        }
        return EErrorClass::RetryableAbort;
    }
    if (dynamic_cast<const pqxx::broken_connection*>(&ex) != nullptr) {
        return EErrorClass::NotCommitted;
    }
    const auto state = PgSqlStateOf(ex);
    if (!state.empty()) {
        return Classify(state, ex.what());
    }
    return Classify({}, ex.what());
}

EErrorClass TPgErrorClassifier::ClassifyCommitException(const std::exception& ex) const {
    if (dynamic_cast<const pqxx::broken_connection*>(&ex) != nullptr) {
        return EErrorClass::AmbiguousCommit;
    }
    const auto state = PgSqlStateOf(ex);
    if (state.size() >= 2 && state.substr(0, 2) == "08") {
        return EErrorClass::AmbiguousCommit;
    }
    return ClassifyException(ex);
}

} // namespace NTpcc
