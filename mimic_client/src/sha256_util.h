#pragma once
#include <string>
#include <cstddef>

// Lowercase-hex SHA-256 of a byte buffer. Empty string on failure.
std::string sha256_hex(const void* data, size_t len);

// Lowercase-hex SHA-256 of a file's contents. Empty string on failure/missing.
std::string sha256_hex_file(const char* path);
