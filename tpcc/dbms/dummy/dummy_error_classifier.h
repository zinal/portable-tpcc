#pragma once

#include <error_classifier.h>

namespace NTpcc {

class TDummyErrorClassifier : public IErrorClassifier {
public:
    EErrorClass Classify(
        std::string_view nativeCode,
        std::string_view message = {}) const override;
};

} // namespace NTpcc
