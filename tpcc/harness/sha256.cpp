#include "sha256.h"

#include <openssl/evp.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace NTpcc {

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string HexEncode(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return out;
}

} // anonymous

std::string ComputeBytesSha256Hex(const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 digest failed");
    }
    EVP_MD_CTX_free(ctx);
    return HexEncode(hash, hashLen);
}

std::string ComputeFileSha256Hex(const std::string& path) {
    return ComputeBytesSha256Hex(ReadFile(path));
}

} // namespace NTpcc
