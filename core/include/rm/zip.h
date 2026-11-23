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

/// Unpacks a ZIP into `dest` (made if missing), each entry at its path below it, folders as needed:
/// stored and deflated entries, each checked against its CRC-32 before it is written. Refuses the
/// whole archive, before writing anything, when an entry's path would leave `dest` (absolute, a
/// drive, "..") or it is encrypted, ZIP64 or of another method; an entry over `entry_limit` bytes
/// (0 = no limit) fails it too. One entry is in memory at a time. `files` gets the files written.
Status ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::uint64_t entry_limit = 0,
                  std::size_t* files = nullptr);

}  // namespace rm
