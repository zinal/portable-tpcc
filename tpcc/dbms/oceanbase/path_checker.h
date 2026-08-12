#pragma once

#include <string>

namespace NTpcc {

// Pre-flight DB checks. Throw std::runtime_error on failure so orchestrated
// roles can still write artifact-manifest.json (std::exit would skip that).
void CheckDbForInit(const std::string& connectionString, const std::string& path = {});
void CheckDbForImport(const std::string& connectionString, const std::string& path = {});
void CheckDbForIndexes(const std::string& connectionString, const std::string& path = {});
void CheckDbForRun(
    const std::string& connectionString,
    int expectedWhCount,
    const std::string& path = {});

} // namespace NTpcc
