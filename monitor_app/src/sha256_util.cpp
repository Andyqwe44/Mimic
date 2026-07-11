#define _CRT_SECURE_NO_WARNINGS
#include "sha256_util.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

// SHA-256 via CNG (bcrypt). Used by download-update integrity check and the
// startup updater self-heal check. Files here are at most a few MB.
std::string sha256_hex(const void* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return "";

    std::string result;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0) == 0) {
            UCHAR digest[32];
            if (BCryptFinishHash(hHash, digest, sizeof(digest), 0) == 0) {
                char hex[65];
                for (int i = 0; i < 32; i++)
                    snprintf(hex + i * 2, 3, "%02x", digest[i]);
                result.assign(hex, 64);
            }
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

std::string sha256_hex_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    std::vector<unsigned char> buf;
    unsigned char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf.insert(buf.end(), chunk, chunk + n);
    fclose(f);
    return sha256_hex(buf.data(), buf.size());
}
