#include <gtest/gtest.h>

#include <password_secret.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>

using namespace NTpcc;

TEST(PasswordSecret, ReadFromFilePrefersFileOverEnv) {
    const std::string path = "password_secret_ut.pwd";
    {
        std::ofstream out(path, std::ios::trunc);
        out << "from-file\n";
    }
    setenv("PASSWORD_SECRET_UT", "from-env", 1);
    EXPECT_EQ(ReadDatabasePassword(path, "PASSWORD_SECRET_UT", ""), "from-file");
    std::remove(path.c_str());
    unsetenv("PASSWORD_SECRET_UT");
}

TEST(PasswordSecret, ReadFromEnvWhenFileEmpty) {
    setenv("PASSWORD_SECRET_UT", "env-only", 1);
    EXPECT_EQ(ReadDatabasePassword("", "PASSWORD_SECRET_UT", ""), "env-only");
    unsetenv("PASSWORD_SECRET_UT");
}

TEST(PasswordSecret, ResolveRelativeUnderRunDir) {
    const std::string runDir = "password_secret_ut_dir";
    const std::string path = runDir + "/db-password";
    ASSERT_EQ(mkdir(runDir.c_str(), 0755), 0);
    {
        std::ofstream out(path, std::ios::trunc);
        out << "rel-secret";
    }
    EXPECT_EQ(ReadDatabasePassword("db-password", "", runDir), "rel-secret");
    std::remove(path.c_str());
    rmdir(runDir.c_str());
}

TEST(PasswordSecret, MissingBothThrows) {
    EXPECT_THROW(ReadDatabasePassword("", "", ""), std::runtime_error);
}
