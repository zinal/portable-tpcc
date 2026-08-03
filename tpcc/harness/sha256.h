#pragma once

#include <string>

namespace NTpcc {

std::string ComputeFileSha256Hex(const std::string& path);
std::string ComputeBytesSha256Hex(const std::string& data);

} // namespace NTpcc
