#pragma once

#include <capabilities.h>

namespace NTpcc {

class TDummyCapabilities : public ICapabilities {
public:
    TCapabilities Get() const override;
};

} // namespace NTpcc
