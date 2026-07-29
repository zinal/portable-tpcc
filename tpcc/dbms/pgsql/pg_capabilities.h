#pragma once

#include <capabilities.h>

namespace NTpcc {

class TPgCapabilities : public ICapabilities {
public:
    TCapabilities Get() const override;
};

} // namespace NTpcc
