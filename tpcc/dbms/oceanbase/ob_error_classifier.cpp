#include "ob_error_classifier.h"

#include <cstdlib>

namespace NTpcc {

namespace {

int ParseCode(std::string_view nativeCode) {
    if (nativeCode.empty()) {
        return 0;
    }
    return std::atoi(std::string(nativeCode).c_str());
}

EErrorClass ClassifyCode(int code, bool commit) {
    switch (code) {
        case 1213:
        case 1205:
        case 6235:
        case 6002:
            return EErrorClass::RetryableAbort;
        case 1317:
            return EErrorClass::Cancelled;
        case 2006:
        case 2013:
            return commit ? EErrorClass::AmbiguousCommit : EErrorClass::NotCommitted;
        default:
            return EErrorClass::Permanent;
    }
}

} // namespace

std::string ObNativeCodeOf(const std::exception& ex) {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return std::to_string(db->Code());
    }
    return {};
}

EErrorClass TObErrorClassifier::Classify(
    std::string_view nativeCode,
    std::string_view /*message*/) const
{
    return ClassifyCode(ParseCode(nativeCode), false);
}

EErrorClass TObErrorClassifier::ClassifyException(const std::exception& ex) const {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return ClassifyCode(db->Code(), false);
    }
    return EErrorClass::Permanent;
}

EErrorClass TObErrorClassifier::ClassifyCommitException(const std::exception& ex) const {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return ClassifyCode(db->Code(), true);
    }
    return EErrorClass::Permanent;
}

} // namespace NTpcc
