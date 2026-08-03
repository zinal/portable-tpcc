#pragma once

#include "ob_errors.h"

#include <error_classifier.h>

#include <exception>
#include <string>

namespace NTpcc {

class TObErrorClassifier : public IErrorClassifier {
public:
    EErrorClass Classify(
        std::string_view nativeCode,
        std::string_view message = {}) const override;

    EErrorClass ClassifyException(const std::exception& ex) const override;
    EErrorClass ClassifyCommitException(const std::exception& ex) const;
};

std::string ObNativeCodeOf(const std::exception& ex);

} // namespace NTpcc
