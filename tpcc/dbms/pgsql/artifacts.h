#pragma once

#include "run_config.h"
#include "terminal.h"

#include <chrono>
#include <string>
#include <vector>

namespace NTpcc {

struct TArtifactPaths {
    std::string InstanceDir;
    std::string ProcessJson;
    std::string ReadyJson;
    std::string ResultJson;
    std::string ArtifactManifestJson;
    std::string StdoutLog;
    std::string StderrLog;
};

TArtifactPaths MakeArtifactPaths(const std::string& instanceDir);

void EnsureInstanceDir(const std::string& instanceDir);
void WriteProcessJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                      const std::string& instance, const std::string& role, int pid,
                      const std::string& instanceNonce);
void WriteReadyJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                    const std::string& instance, const std::vector<TWarehouseRange>& ranges,
                    const std::string& instanceNonce);
void WriteLoaderResultJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                           const std::string& instance, const TLoaderAssignment& assign, int exitCode);
void WriteWorkerResultJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                           const std::string& instance, const TWorkerAssignment& assign,
                           const TTerminalStats& stats, bool highResHistogram,
                           std::chrono::system_clock::time_point rampStart,
                           std::chrono::system_clock::time_point measurementStart,
                           std::chrono::system_clock::time_point measurementEnd,
                           std::chrono::system_clock::time_point drainDeadline,
                           double measureSeconds, int exitCode,
                           const std::string& instanceNonce);
void WriteArtifactManifest(const TArtifactPaths& paths, const std::string& instance,
                           const std::string& instanceNonce, int exitCode);

std::string GenerateInstanceNonce();

} // namespace NTpcc
