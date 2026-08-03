#pragma once

#include <capabilities.h>

#include <string>

namespace NTpcc {

class TPgCapabilities : public ICapabilities {
public:
    explicit TPgCapabilities(std::string partitioningStyle = "none");

    TCapabilities Get() const override;

private:
    std::string PartitioningStyle_;
};

} // namespace NTpcc
