#include "rm/zip.h"

#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include "rm/common.h"

namespace rm {
namespace {

constexpr std::array<std::uint32_t, 256> MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = MakeCrcTable();

void Put16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

void Put32(std::string& out, std::uint32_t v) {
    Put16(out, v & 0xFFFFu);
    Put16(out, (v >> 16) & 0xFFFFu);
}

// DOS date 1980-01-01, time 00:00:00 -> reproducible archives.
constexpr std::uint32_t kDosTime = 0;
constexpr std::uint32_t kDosDate = (0u << 9) | (1u << 5) | 1u;
constexpr std::uint32_t kVersionNeeded = 20;  // 2.0: plain stored files
constexpr std::uint32_t kFlagUtf8 = 0x0800;

}  // namespace

std::uint32_t Crc32(std::string_view data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char ch : data) c = kCrcTable[(c ^ static_cast<unsigned char>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

Status WriteStoredZip(const std::filesystem::path& path, const std::vector<ZipEntry>& files) {
    constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();
    if (files.size() > 0xFFFFu) return Status::Error(ErrorCode::InvalidArgument, "too many ZIP entries");

    std::string archive;
    std::string central;
    for (const ZipEntry& f : files) {
        std::string name = f.name;
        for (char& c : name) {
            if (c == '\\') c = '/';
        }
        if (name.empty() || name.front() == '/' || name.size() > 0xFFFFu || name.find("..") != std::string::npos) {
            return Status::Error(ErrorCode::InvalidArgument, "invalid ZIP entry name '" + f.name + "'");
        }
        if (f.data.size() > kMax32 || archive.size() > kMax32) {
            return Status::Error(ErrorCode::InvalidArgument, "ZIP entry too large (ZIP64 unsupported)");
        }
        const auto offset = static_cast<std::uint32_t>(archive.size());
        const auto size = static_cast<std::uint32_t>(f.data.size());
        const std::uint32_t crc = Crc32(f.data);
        const auto name_len = static_cast<std::uint32_t>(name.size());

        Put32(archive, 0x04034B50u);  // local file header
        Put16(archive, kVersionNeeded);
        Put16(archive, kFlagUtf8);
        Put16(archive, 0);  // method: stored
        Put16(archive, kDosTime);
        Put16(archive, kDosDate);
        Put32(archive, crc);
        Put32(archive, size);  // compressed
        Put32(archive, size);  // uncompressed
        Put16(archive, name_len);
        Put16(archive, 0);  // extra length
        archive += name;
        archive += f.data;

        Put32(central, 0x02014B50u);  // central directory header
        Put16(central, kVersionNeeded);  // version made by (MS-DOS, 2.0)
        Put16(central, kVersionNeeded);
        Put16(central, kFlagUtf8);
        Put16(central, 0);
        Put16(central, kDosTime);
        Put16(central, kDosDate);
        Put32(central, crc);
        Put32(central, size);
        Put32(central, size);
        Put16(central, name_len);
        Put16(central, 0);  // extra
        Put16(central, 0);  // comment
        Put16(central, 0);  // disk number start
        Put16(central, 0);  // internal attributes
        Put32(central, 0);  // external attributes
        Put32(central, offset);
        central += name;
    }
    if (archive.size() > kMax32 || central.size() > kMax32) {
        return Status::Error(ErrorCode::InvalidArgument, "ZIP archive too large (ZIP64 unsupported)");
    }
    const auto cd_offset = static_cast<std::uint32_t>(archive.size());
    const auto cd_size = static_cast<std::uint32_t>(central.size());
    const auto count = static_cast<std::uint32_t>(files.size());
    archive += central;
    Put32(archive, 0x06054B50u);  // end of central directory
    Put16(archive, 0);
    Put16(archive, 0);
    Put16(archive, count);
    Put16(archive, count);
    Put32(archive, cd_size);
    Put32(archive, cd_offset);
    Put16(archive, 0);  // comment length

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return Status::Error(ErrorCode::IoError, "cannot open " + rm::PathText(tmp));
        out.write(archive.data(), static_cast<std::streamsize>(archive.size()));
        out.flush();
        if (!out) return Status::Error(ErrorCode::IoError, "cannot write " + rm::PathText(tmp));
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return Status::Error(ErrorCode::IoError, "cannot replace " + rm::PathText(path) + ": " + ec.message());
    return Status::Success();
}

}  // namespace rm
