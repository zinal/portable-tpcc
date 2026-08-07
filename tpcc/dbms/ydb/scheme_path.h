#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace NTpcc {

// Resolve --path to an absolute YDB path. Relative paths are rooted at database.
inline std::string ResolveYdbAbsolutePath(
    const std::string& database, const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    if (path.front() != '/') {
        return database + "/" + path;
    }
    return path;
}

// True when path equals database or is a nested path under it (prefix + '/').
inline bool IsYdbPathInsideDatabase(
    const std::string& database, const std::string& path)
{
    if (path.empty() || path == database) {
        return true;
    }
    return path.size() > database.size()
        && path.compare(0, database.size(), database) == 0
        && path[database.size()] == '/';
}

// Directory paths that must be created for schema init.
// Only returns components strictly under database; never parents of database.
inline std::vector<std::string> YdbDirectoriesToCreate(
    const std::string& database, const std::string& absolutePath)
{
    std::vector<std::string> dirs;
    if (absolutePath.empty() || absolutePath == database) {
        return dirs;
    }
    if (!IsYdbPathInsideDatabase(database, absolutePath)) {
        throw std::runtime_error(
            "YDB path must be inside the database: path=" + absolutePath
            + ", database=" + database);
    }

    size_t pos = database.size();
    while (pos < absolutePath.size()) {
        const size_t next = absolutePath.find('/', pos + 1);
        const std::string current = absolutePath.substr(
            0, next == std::string::npos ? absolutePath.size() : next);
        if (current.size() > database.size()) {
            dirs.push_back(current);
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next;
    }
    return dirs;
}

} // namespace NTpcc
