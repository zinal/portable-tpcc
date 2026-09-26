#include "version.h"

#include <build/scripts/c_templates/svnversion.h>

#include <cctype>
#include <iostream>
#include <mutex>

namespace NTpcc {

namespace {

constexpr size_t kShortCommitHexDigits = 12;

bool IsHexString(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (unsigned char c : id) {
        if (!std::isxdigit(c)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string ShortCommitForm(const std::string& id) {
    if (id.empty()) {
        return "unknown";
    }
    if (IsHexString(id) && id.size() > kShortCommitHexDigits) {
        return id.substr(0, kShortCommitHexDigits);
    }
    return id;
}

std::string ShortCommitId() {
    const char* id = GetProgramCommitId();
    if (id == nullptr || id[0] == '\0') {
        id = GetProgramHash();
    }
    if (id == nullptr) {
        return ShortCommitForm("");
    }
    return ShortCommitForm(id);
}

std::string FormatModuleCommitLine(const std::string& role, const std::string& instance,
                                   const std::string& commit) {
    std::string who = role;
    if (!instance.empty()) {
        if (!who.empty()) {
            who.push_back('/');
        }
        who += instance;
    }
    if (who.empty()) {
        who = "module";
    }
    return "module " + who + " commit " + commit;
}

void AnnounceModuleCommit(const std::string& role, const std::string& instance) {
    static std::once_flag once;
    std::call_once(once, [&] {
        std::cout << FormatModuleCommitLine(role, instance, ShortCommitId()) << std::endl;
    });
}

} // namespace NTpcc
