#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace NTpcc {

enum class ECheckPhase {
    AfterImport,
    AfterTest,
    Both,
};

struct TCheckCatalogEntry {
    std::string_view Id;
    std::string_view Title;
    ECheckPhase Phase = ECheckPhase::Both;
};

// Shared TPC-C integrity check catalog (IDs + expected semantics).
// Adapters evaluate each applicable entry against the live database.
const std::vector<TCheckCatalogEntry>& CheckCatalog();

const TCheckCatalogEntry* FindCheckCatalogEntry(std::string_view id);

bool CheckAppliesToPhase(ECheckPhase entryPhase, ECheckPhase requested);

// Catalog ids that apply to `requested` (Both + the matching phase).
int CountCatalogChecks(ECheckPhase requested);

} // namespace NTpcc
