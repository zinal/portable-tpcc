#include <password_secret.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace NTpcc {

namespace {

std::string ResolveUnderRunDir(const std::string& path, const std::string& runDir) {
    if (path.empty() || runDir.empty()) {
        return path;
    }
    if (path.front() == '/' || (path.size() >= 2 && path[1] == ':')) {
        return path;
    }
    return runDir + "/" + path;
}

std::string StripTrailingNewlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::string ReadFileContents(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open password file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return StripTrailingNewlines(ss.str());
}

} // anonymous

std::string ReadDatabasePassword(
    const std::string& passwordFile,
    const std::string& passwordEnv,
    const std::string& runDir)
{
    if (!passwordFile.empty()) {
        return ReadFileContents(ResolveUnderRunDir(passwordFile, runDir));
    }
    if (passwordEnv.empty()) {
        throw std::runtime_error(
            "database.password_file or database.password_env is required");
    }
    const char* value = std::getenv(passwordEnv.c_str());
    if (!value) {
        throw std::runtime_error("environment variable not set: " + passwordEnv);
    }
    return value;
}

} // namespace NTpcc
