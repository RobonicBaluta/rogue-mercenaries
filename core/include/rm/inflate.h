#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rm {

/// Decodes a zlib stream (RFC 1950 header, DEFLATE body per RFC 1951, Adler-32 trailer) into
/// `out` (replaced). False on malformed input, a preset dictionary, a checksum mismatch or more
/// than `limit` bytes of output (0 = no limit). Used for game archives whose entries are zlib.
bool ZlibInflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::size_t limit = 0);

}  // namespace rm
