#include <gtest/gtest.h>

#include <artifacts.h>
#include <nlohmann/json.hpp>
#include <version.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace NTpcc;
namespace fs = std::filesystem;

TEST(ModuleCommit, ShortForm) {
    EXPECT_EQ(ShortCommitForm(""), "unknown");
    EXPECT_EQ(ShortCommitForm("2902074"), "2902074");
    EXPECT_EQ(ShortCommitForm("0123456789ab"), "0123456789ab");
    EXPECT_EQ(ShortCommitForm("0123456789abcdef0123456789abcdef01234567"), "0123456789ab");
    EXPECT_EQ(ShortCommitForm("not-a-hash-but-long-enough"), "not-a-hash-but-long-enough");
}

TEST(ModuleCommit, FormatLine) {
    EXPECT_EQ(FormatModuleCommitLine("worker", "worker-0", "0123456789ab"),
              "module worker/worker-0 commit 0123456789ab");
    EXPECT_EQ(FormatModuleCommitLine("run", "", "0123456789ab"),
              "module run commit 0123456789ab");
}

TEST(ModuleCommit, CurrentCommitIsNonEmpty) {
    const std::string id = ShortCommitId();
    EXPECT_FALSE(id.empty());
    EXPECT_LE(id.size(), 12u);
}

TEST(ModuleCommit, ProcessJsonRecordsCommit) {
    const fs::path dir = fs::temp_directory_path() / "tpcc-module-commit-ut";
    fs::remove_all(dir);
    EnsureInstanceDir(dir.string());
    const auto paths = MakeArtifactPaths(dir.string());

    TRunConfigDocument doc;
    doc.RunId = "run-1";
    doc.RunConfigSha256 = "abc";
    WriteProcessJson(paths, doc, "worker-0", "worker", 42, "nonce-1");

    std::ifstream in(paths.ProcessJson);
    ASSERT_TRUE(in.good());
    nlohmann::json parsed;
    in >> parsed;
    EXPECT_EQ(parsed.at("commit").get<std::string>(), ShortCommitId());
    EXPECT_EQ(parsed.at("role").get<std::string>(), "worker");
    EXPECT_EQ(parsed.at("instance").get<std::string>(), "worker-0");

    fs::remove_all(dir);
}
