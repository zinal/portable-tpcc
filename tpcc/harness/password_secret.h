#pragma once

#include <string>

namespace NTpcc {

// Resolve a DB password for orchestrated/standalone adapters.
// Prefer password_file (worker-local path, optionally under runDir); otherwise
// read the environment variable named by passwordEnv. Trailing newlines are
// stripped from file contents so deploy/write round-trips stay predictable.
std::string ReadDatabasePassword(
    const std::string& passwordFile,
    const std::string& passwordEnv,
    const std::string& runDir = std::string());

} // namespace NTpcc
