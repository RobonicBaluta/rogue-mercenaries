#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rm/status.h"

namespace rm {

struct ZipEntry {
    std::string name;  // archive path with '/' separators, UTF-8
    std::string data;  // raw bytes
};

std::uint32_t Crc32(std::string_view data);

/// Writes an uncompressed (method 0, "stored") ZIP with correct local headers, central
/// directory and end record. No ZIP64: each entry and the archive must stay below 4 GiB
/// and 65535 entries. Timestamps are fixed (1980-01-01) so archives are reproducible.
Status WriteStoredZip(const std::filesystem::path& path, const std::vector<ZipEntry>& files);

}  // namespace rm
