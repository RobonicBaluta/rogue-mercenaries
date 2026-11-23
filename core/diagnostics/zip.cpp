#include "rm/zip.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include "rm/common.h"
#include "rm/inflate.h"

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

namespace {

std::uint32_t Get16(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t Get32(const std::uint8_t* p) { return Get16(p) | (Get16(p + 2) << 16); }

struct CentralEntry {
    std::string name;
    std::uint32_t flags = 0, method = 0, crc = 0, packed = 0, size = 0, offset = 0;
};

// A ZIP entry's name as a path below the destination, or nothing when it would leave it.
std::optional<std::filesystem::path> SafeRelative(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    if (name.empty() || name.front() == '/') return std::nullopt;
    std::filesystem::path out;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = std::min(name.find('/', start), name.size());
        const std::string part = name.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find(':') != std::string::npos) return std::nullopt;
        out /= std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(part.data()), part.size()));
    }
    if (out.empty()) return std::nullopt;
    return out;
}

bool ReadAt(std::ifstream& in, std::uint64_t at, std::uint8_t* to, std::size_t n) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char*>(to), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(in.gcount()) == n;
}

}  // namespace

Status ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& dest, std::uint64_t entry_limit, std::size_t* files) {
    if (files) *files = 0;
    std::ifstream in(zip, std::ios::binary);
    if (!in) return Status::Error(ErrorCode::IoError, "cannot open " + PathText(zip));
    std::error_code ec;
    const std::uint64_t total = std::filesystem::file_size(zip, ec);
    if (ec || total < 22) return Status::Error(ErrorCode::ParseError, "not a ZIP");

    // The end record: within the last 64 KiB + 22 bytes (its comment's room).
    const std::size_t tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(total, 0xFFFFu + 22u));
    std::vector<std::uint8_t> tail(tailSize);
    if (!ReadAt(in, total - tailSize, tail.data(), tailSize)) return Status::Error(ErrorCode::IoError, "cannot read " + PathText(zip));
    std::size_t eocd = std::string::npos;
    for (std::size_t i = tailSize - 22 + 1; i-- > 0;) {
        if (Get32(&tail[i]) == 0x06054B50u) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return Status::Error(ErrorCode::ParseError, "not a ZIP (no end record)");
    const std::uint8_t* e = &tail[eocd];
    const std::uint32_t count = Get16(e + 10), cdSize = Get32(e + 12), cdOffset = Get32(e + 16);
    if (Get16(e + 4) != 0 || Get16(e + 6) != 0 || Get16(e + 8) != count) return Status::Error(ErrorCode::ParseError, "a ZIP split in parts");
    if (count == 0xFFFFu || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) return Status::Error(ErrorCode::ParseError, "a ZIP64 archive");
    if (static_cast<std::uint64_t>(cdOffset) + cdSize > total) return Status::Error(ErrorCode::ParseError, "a broken ZIP (central directory)");

    std::vector<std::uint8_t> cd(cdSize);
    if (cdSize > 0 && !ReadAt(in, cdOffset, cd.data(), cdSize)) return Status::Error(ErrorCode::IoError, "cannot read " + PathText(zip));
    std::vector<CentralEntry> entries;
    for (std::size_t at = 0, i = 0; i < count; ++i) {
        if (at + 46 > cd.size() || Get32(&cd[at]) != 0x02014B50u) return Status::Error(ErrorCode::ParseError, "a broken ZIP (entry)");
        const std::uint8_t* c = &cd[at];
        CentralEntry entry;
        entry.flags = Get16(c + 8);
        entry.method = Get16(c + 10);
        entry.crc = Get32(c + 16);
        entry.packed = Get32(c + 20);
        entry.size = Get32(c + 24);
        const std::uint32_t nameLen = Get16(c + 28), extraLen = Get16(c + 30), commentLen = Get16(c + 32);
        entry.offset = Get32(c + 42);
        if (at + 46 + nameLen > cd.size()) return Status::Error(ErrorCode::ParseError, "a broken ZIP (name)");
        entry.name.assign(reinterpret_cast<const char*>(c + 46), nameLen);
        at += 46 + nameLen + extraLen + commentLen;
        if (entry.packed == 0xFFFFFFFFu || entry.size == 0xFFFFFFFFu || entry.offset == 0xFFFFFFFFu) {
            return Status::Error(ErrorCode::ParseError, "a ZIP64 entry: " + entry.name);
        }
        if (entry.flags & 1u) return Status::Error(ErrorCode::ParseError, "an encrypted entry: " + entry.name);
        const bool folder = !entry.name.empty() && (entry.name.back() == '/' || entry.name.back() == '\\');
        if (!folder && entry.method != 0 && entry.method != 8) {
            return Status::Error(ErrorCode::ParseError, "an entry packed another way (method " + std::to_string(entry.method) + "): " + entry.name);
        }
        if (!SafeRelative(entry.name)) return Status::Error(ErrorCode::InvalidArgument, "an entry outside its folder: " + entry.name);
        if (entry_limit > 0 && entry.size > entry_limit) return Status::Error(ErrorCode::InvalidArgument, "an entry too big: " + entry.name);
        entries.push_back(std::move(entry));
    }

    std::filesystem::create_directories(dest, ec);
    std::vector<std::uint8_t> packed;
    std::vector<std::uint8_t> plain;
    for (const CentralEntry& entry : entries) {
        const std::filesystem::path target = dest / *SafeRelative(entry.name);
        if (entry.name.back() == '/' || entry.name.back() == '\\') {
            std::filesystem::create_directories(target, ec);
            continue;
        }
        std::uint8_t local[30];
        if (!ReadAt(in, entry.offset, local, sizeof(local)) || Get32(local) != 0x04034B50u) {
            return Status::Error(ErrorCode::ParseError, "a broken ZIP (local header): " + entry.name);
        }
        const std::uint64_t data = static_cast<std::uint64_t>(entry.offset) + 30u + Get16(local + 26) + Get16(local + 28);
        if (data + entry.packed > total) return Status::Error(ErrorCode::ParseError, "a broken ZIP (data): " + entry.name);
        packed.resize(entry.packed);
        if (entry.packed > 0 && !ReadAt(in, data, packed.data(), packed.size())) return Status::Error(ErrorCode::IoError, "cannot read " + entry.name);
        if (entry.method == 0) {
            if (entry.packed != entry.size) return Status::Error(ErrorCode::ParseError, "a broken ZIP (stored size): " + entry.name);
            plain.swap(packed);
        } else if (!RawInflate(packed.data(), packed.size(), plain, entry.size) || plain.size() != entry.size) {
            return Status::Error(ErrorCode::ParseError, "a broken ZIP (deflate): " + entry.name);
        }
        const std::string_view bytes(reinterpret_cast<const char*>(plain.data()), plain.size());
        if (Crc32(bytes) != entry.crc) return Status::Error(ErrorCode::ParseError, "a damaged entry (CRC): " + entry.name);
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) return Status::Error(ErrorCode::IoError, "cannot write " + PathText(target));
        if (files) ++*files;
    }
    return Status::Success();
}

}  // namespace rm
