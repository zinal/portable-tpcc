#pragma once

#include <capabilities.h>

namespace NTpcc {

class TYdbCapabilities final : public ICapabilities {
public:
    TCapabilities Get() const override;
};

} // namespace NTpcc
