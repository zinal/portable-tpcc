#include "catalog.h"

namespace NTpcc {

namespace {

const std::vector<TCheckCatalogEntry>& Catalog() {
    static const std::vector<TCheckCatalogEntry> kCatalog = {
        {"cardinality.warehouse", "Warehouse cardinality", ECheckPhase::Both},
        {"cardinality.district", "District cardinality", ECheckPhase::Both},
        {"cardinality.customer", "Customer cardinality", ECheckPhase::Both},
        {"cardinality.item", "Item cardinality", ECheckPhase::Both},
        {"cardinality.stock", "Stock cardinality", ECheckPhase::Both},
        {"cardinality.oorder", "Order cardinality (post-import)", ECheckPhase::AfterImport},
        {"cardinality.new_order", "New-order cardinality (post-import)", ECheckPhase::AfterImport},
        {"cardinality.order_line", "Order-line cardinality (post-import)", ECheckPhase::AfterImport},
        {"cardinality.history", "History cardinality (post-import)", ECheckPhase::AfterImport},
        {"consistency.3.3.2.1", "W_YTD equals sum(D_YTD)", ECheckPhase::Both},
        {"consistency.3.3.2.2", "D_NEXT_O_ID vs max order ids", ECheckPhase::Both},
        {"consistency.3.3.2.3", "New-order id range contiguous", ECheckPhase::Both},
        {"consistency.3.3.2.4", "sum(O_OL_CNT) equals order-line count", ECheckPhase::Both},
        {"consistency.3.3.2.5", "New-order ↔ undelivered order pairing", ECheckPhase::Both},
        {"consistency.3.3.2.6", "Order line count matches O_OL_CNT", ECheckPhase::Both},
        {"consistency.3.3.2.7", "Carrier vs delivery-date consistency", ECheckPhase::Both},
        {"consistency.3.3.2.8", "W_YTD equals sum(H_AMOUNT)", ECheckPhase::Both},
        {"consistency.3.3.2.9", "D_YTD equals sum(H_AMOUNT)", ECheckPhase::Both},
        {"consistency.3.3.2.10", "C_BALANCE vs delivered amounts and history", ECheckPhase::Both},
        {"consistency.3.3.2.11", "Orders vs new-orders delta (post-import)", ECheckPhase::AfterImport},
        {"consistency.3.3.2.12", "C_BALANCE + C_YTD_PAYMENT vs delivered amounts", ECheckPhase::Both},
        {"post_import.d_next_o_id", "D_NEXT_O_ID initial value", ECheckPhase::AfterImport},
        {"post_import.w_ytd", "W_YTD initial value", ECheckPhase::AfterImport},
        {"post_import.d_ytd", "D_YTD initial value", ECheckPhase::AfterImport},
        {"post_import.o_carrier_id", "Unprocessed orders have NULL carrier", ECheckPhase::AfterImport},
        {"post_import.ol_delivery_d", "Unprocessed lines have NULL delivery date", ECheckPhase::AfterImport},
        {"post_import.ol_delivery_eq_entry", "Delivered lines have OL_DELIVERY_D = O_ENTRY_D", ECheckPhase::AfterImport},
    };
    return kCatalog;
}

} // anonymous

const std::vector<TCheckCatalogEntry>& CheckCatalog() {
    return Catalog();
}

const TCheckCatalogEntry* FindCheckCatalogEntry(std::string_view id) {
    for (const auto& e : Catalog()) {
        if (e.Id == id) {
            return &e;
        }
    }
    return nullptr;
}

bool CheckAppliesToPhase(ECheckPhase entryPhase, ECheckPhase requested) {
    if (entryPhase == ECheckPhase::Both) {
        return true;
    }
    return entryPhase == requested;
}

} // namespace NTpcc
