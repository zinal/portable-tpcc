#pragma once

#include <string>

namespace NTpcc {

// Short VCS commit id: a longer hexadecimal hash is cut to 12 digits.
// An empty identifier becomes "unknown".
std::string ShortCommitForm(const std::string& id);

// Commit id of this binary (build-time VCS info).
std::string ShortCommitId();

// One stdout line: `module <role>/<instance> commit <id>`.
std::string FormatModuleCommitLine(const std::string& role, const std::string& instance,
                                   const std::string& commit);

// Print FormatModuleCommitLine once per process, at the first role start.
void AnnounceModuleCommit(const std::string& role, const std::string& instance);

} // namespace NTpcc
