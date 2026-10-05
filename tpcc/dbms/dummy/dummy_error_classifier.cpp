#include "dummy_error_classifier.h"

namespace NTpcc {

EErrorClass TDummyErrorClassifier::Classify(
    std::string_view /*nativeCode*/,
    std::string_view /*message*/) const
{
    return EErrorClass::Permanent;
}

} // namespace NTpcc
