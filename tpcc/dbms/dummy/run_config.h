#pragma once

#include "dummy_delay.h"

#include <run_config_document.h>

#include <string>

namespace NTpcc {

TRunConfigDocument LoadRunConfigDocument(const std::string& path);
TDummyDelayConfig DelayConfigFromDocument(const TRunConfigDocument& doc);

} // namespace NTpcc
