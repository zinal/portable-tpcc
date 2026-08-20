#include <gtest/gtest.h>

#include <run_config.h>
#include <run_config_document.h>

#include <cstdlib>
#include <string>

using namespace NTpcc;

namespace {

TRunConfigDocument BaseDoc() {
    TRunConfigDocument doc;
    doc.Endpoint = "db.example:5433";
    doc.Database = "tpcc";
    doc.PasswordEnv = "TPCC_TEST_PG_PASSWORD";
    return doc;
}

void SetTestPassword() {
    ASSERT_EQ(setenv("TPCC_TEST_PG_PASSWORD", "s3cret", 1), 0);
}

} // namespace

TEST(PgConnectionString, UsesDatabaseUser) {
    SetTestPassword();
    auto doc = BaseDoc();
    doc.User = "bench";
    const std::string conn = BuildPgConnectionString(doc);
    EXPECT_NE(conn.find("user='bench'"), std::string::npos);
    EXPECT_EQ(conn.find("user='postgres'"), std::string::npos);
    EXPECT_NE(conn.find("host='db.example'"), std::string::npos);
    EXPECT_NE(conn.find("port='5433'"), std::string::npos);
    EXPECT_NE(conn.find("dbname='tpcc'"), std::string::npos);
}

TEST(PgConnectionString, DefaultsToPostgresWhenUserOmitted) {
    SetTestPassword();
    const std::string conn = BuildPgConnectionString(BaseDoc());
    EXPECT_NE(conn.find("user='postgres'"), std::string::npos);
}
