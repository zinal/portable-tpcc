#include "dummy_admin_adapter.h"

#include <log.h>

namespace NTpcc {

void TDummyAdminAdapter::EnsureSchema() {
    LOG_I("Dummy schema: no-op");
}

void TDummyAdminAdapter::EnsureIndexes() {
    LOG_I("Dummy indexes: no-op");
}

void TDummyAdminAdapter::EnsureStatistics() {
    LOG_I("Dummy statistics: no-op");
}

void TDummyAdminAdapter::Clean() {
    LOG_I("Dummy drop: no-op");
}

TAdminDescribe TDummyAdminAdapter::Describe() {
    TAdminDescribe d;
    d.AdapterName = "dummy";
    d.ServerVersion = "dummy";
    d.ClientVersion = "dummy";
    return d;
}

} // namespace NTpcc
