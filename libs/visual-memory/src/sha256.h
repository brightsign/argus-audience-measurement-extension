// SPDX-License-Identifier: MIT
// Minimal self-contained SHA-256 (internal). Public-domain style reference
// implementation — no external crypto dependency. Used only for stored-JPEG
// integrity hashing, not as evidence of physical display.
#ifndef VISUALMEMORY_SHA256_H
#define VISUALMEMORY_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace visualmemory::detail {

// Return the lowercase hex SHA-256 of `len` bytes at `data`.
std::string sha256_hex(const uint8_t* data, size_t len);

} // namespace visualmemory::detail

#endif // VISUALMEMORY_SHA256_H
