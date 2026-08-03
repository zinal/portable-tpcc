#include "ob_queries.h"

#include <cstddef>

namespace NTpcc {
namespace {

struct TQueryDef {
    std::string_view Sql;
    bool IsSelect = false;
};

constexpr TQueryDef Queries[] = {
    {"SELECT w_tax FROM warehouse WHERE w_id = ?", true},
    {"SELECT d_next_o_id, d_tax FROM district WHERE d_w_id = ? AND d_id = ? FOR UPDATE", true},
    {"UPDATE district SET d_next_o_id = ? WHERE d_w_id = ? AND d_id = ?", false},
    {
        "SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, "
        "c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, "
        "c_payment_cnt, c_delivery_cnt, c_data, c_since "
        "FROM customer WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?",
        true,
    },
    {"SELECT i_id, i_price, i_name, i_data FROM item WHERE i_id = ?", true},
    {
        "INSERT INTO oorder (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) "
        "VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP, ?, ?)",
        false,
    },
    {"INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES (?, ?, ?)", false},
    {
        "UPDATE stock SET s_quantity = ?, s_ytd = s_ytd + ?, s_order_cnt = s_order_cnt + 1, "
        "s_remote_cnt = s_remote_cnt + ? WHERE s_w_id = ? AND s_i_id = ?",
        false,
    },
    {
        "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, "
        "ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        false,
    },
    {"SELECT d_next_o_id FROM district WHERE d_w_id = ? AND d_id = ?", true},
    {
        "SELECT COUNT(DISTINCT s.s_i_id) FROM order_line ol, stock s "
        "WHERE ol.ol_w_id = ? AND ol.ol_d_id = ? AND ol.ol_o_id < ? AND ol.ol_o_id >= ? "
        "AND s.s_w_id = ? AND s.s_i_id = ol.ol_i_id AND s.s_quantity < ?",
        true,
    },
    {
        "SELECT no_o_id FROM new_order WHERE no_w_id = ? AND no_d_id = ? "
        "ORDER BY no_o_id ASC LIMIT 1 FOR UPDATE",
        true,
    },
    {"UPDATE warehouse SET w_ytd = w_ytd + ? WHERE w_id = ?", false},
    {"UPDATE district SET d_ytd = d_ytd + ? WHERE d_w_id = ? AND d_id = ?", false},
    {"SELECT w_name, w_street_1, w_street_2, w_city, w_state, w_zip FROM warehouse WHERE w_id = ?", true},
    {"SELECT d_name, d_street_1, d_street_2, d_city, d_state, d_zip FROM district WHERE d_w_id = ? AND d_id = ?", true},
    {
        "INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data) "
        "VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP, ?, ?)",
        false,
    },
    {
        "SELECT c_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, "
        "c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, "
        "c_payment_cnt, c_delivery_cnt, c_data, c_since "
        "FROM customer WHERE c_w_id = ? AND c_d_id = ? AND c_last = ? ORDER BY c_first",
        true,
    },
    {
        "SELECT s_quantity, s_ytd, s_order_cnt, s_remote_cnt, s_data, "
        "s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, "
        "s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 "
        "FROM stock WHERE s_w_id = ? AND s_i_id = ? FOR UPDATE",
        true,
    },
    {"SELECT c_data FROM customer WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?", true},
    {
        "UPDATE customer SET c_balance = ?, c_ytd_payment = ?, c_payment_cnt = ?, c_data = ? "
        "WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?",
        false,
    },
    {
        "UPDATE customer SET c_balance = ?, c_ytd_payment = ?, c_payment_cnt = ? "
        "WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?",
        false,
    },
    {
        "SELECT o_id, o_c_id, o_carrier_id, o_entry_d FROM oorder "
        "WHERE o_w_id = ? AND o_d_id = ? AND o_c_id = ? ORDER BY o_id DESC LIMIT 1",
        true,
    },
    {
        "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d "
        "FROM order_line WHERE ol_w_id = ? AND ol_d_id = ? AND ol_o_id = ?",
        true,
    },
    {"SELECT o_c_id FROM oorder WHERE o_w_id = ? AND o_d_id = ? AND o_id = ?", true},
    {"SELECT ol_amount FROM order_line WHERE ol_w_id = ? AND ol_d_id = ? AND ol_o_id = ?", true},
    {"DELETE FROM new_order WHERE no_w_id = ? AND no_d_id = ? AND no_o_id = ?", false},
    {"UPDATE oorder SET o_carrier_id = ? WHERE o_w_id = ? AND o_d_id = ? AND o_id = ?", false},
    {"UPDATE order_line SET ol_delivery_d = CURRENT_TIMESTAMP WHERE ol_w_id = ? AND ol_d_id = ? AND ol_o_id = ?", false},
    {
        "UPDATE customer SET c_balance = c_balance + ?, c_delivery_cnt = c_delivery_cnt + 1 "
        "WHERE c_w_id = ? AND c_d_id = ? AND c_id = ?",
        false,
    },
    {"SELECT CAST(? AS SIGNED) AS v", true},
};

static_assert(
    sizeof(Queries) / sizeof(Queries[0]) == static_cast<size_t>(EObQueryId::Count),
    "Queries must have one entry per EObQueryId");

} // namespace

std::string_view QuerySql(EObQueryId id) {
    return Queries[static_cast<size_t>(id)].Sql;
}

bool QueryIsSelect(EObQueryId id) {
    return Queries[static_cast<size_t>(id)].IsSelect;
}

} // namespace NTpcc
