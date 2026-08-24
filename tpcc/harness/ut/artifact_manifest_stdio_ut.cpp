#include <gtest/gtest.h>

#include <artifacts.h>
#include <nlohmann/json.hpp>
#include <sha256.h>

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <util/stream/output.h>

using namespace NTpcc;
namespace fs = std::filesystem;

namespace {

struct TSaveStdio {
    int Out = -1;
    int Err = -1;

    TSaveStdio() {
        Out = ::dup(STDOUT_FILENO);
        Err = ::dup(STDERR_FILENO);
    }

    ~TSaveStdio() {
        if (Out >= 0) {
            ::dup2(Out, STDOUT_FILENO);
            ::close(Out);
        }
        if (Err >= 0) {
            ::dup2(Err, STDERR_FILENO);
            ::close(Err);
        }
    }
};

} // namespace

TEST(ArtifactManifestStdio, SealsStderrSoLaterWritesDoNotChangeHash) {
    TSaveStdio saved;
    const fs::path dir = fs::temp_directory_path() / "tpcc-artifact-stdio-ut";
    fs::remove_all(dir);
    fs::create_directories(dir);

    const std::string stderrPath = (dir / "stderr.log").string();
    const int fileFd = ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(fileFd, 0);
    ASSERT_GE(::dup2(fileFd, STDERR_FILENO), 0);
    ::close(fileFd);

    Cerr << "pre-manifest\n";
    // Intentionally unflushed from the test; WriteArtifactManifest must flush.

    const auto paths = MakeArtifactPaths(dir.string());
    WriteArtifactManifest(paths, "loader-a", "nonce-1", 0);

    Cerr << "post-manifest\n";
    std::fprintf(stderr, "post-fprintf\n");
    Cerr.Flush();
    std::fflush(stderr);

    std::ifstream in(stderrPath, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("pre-manifest"), std::string::npos);
    EXPECT_EQ(body.find("post-manifest"), std::string::npos);
    EXPECT_EQ(body.find("post-fprintf"), std::string::npos);

    std::ifstream mf(paths.ArtifactManifestJson);
    nlohmann::json manifest;
    mf >> manifest;
    bool found = false;
    for (const auto& p : manifest["payloads"]) {
        if (p["path"] != "stderr.log") {
            continue;
        }
        found = true;
        EXPECT_EQ(p["sha256"].get<std::string>(), ComputeFileSha256Hex(stderrPath));
        EXPECT_EQ(p["size"].get<int64_t>(), static_cast<int64_t>(body.size()));
    }
    EXPECT_TRUE(found);

    fs::remove_all(dir);
}
