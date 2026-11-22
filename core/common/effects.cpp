#include "rm/effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rm/inflate.h"

namespace rm::fx {
namespace {

std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
void PutU32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void SetU32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void SetU16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
std::uint8_t To8(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v * 255.0), 0L, 255L)); }

constexpr std::uint32_t kEflVersion = 0x20080415;
constexpr std::size_t kEflHeader = 32;
constexpr std::size_t kPartEntry = 16;
constexpr std::uint16_t kTexVersion = 0x70;
constexpr std::size_t kTexMipTable = 40;
constexpr std::size_t kArcEntry = 80;
constexpr std::size_t kArcPath = 64;
constexpr std::size_t kArcDataStart = 0x8000;
constexpr std::uint32_t kArcSizeFlag = 0x40000000;
constexpr std::uint32_t kArcSizeMask = 0x00FFFFFF;

// A part's colour fields (offsets into its data) by its kind: research/re5-engine-map.md.
void ColourFields(std::uint32_t kind, std::vector<std::size_t>& out) {
    out.assign({12, 72, 76});
    if (kind == 1) out.insert(out.end(), {376, 380});
    if (kind == 4) out.insert(out.end(), {88, 92});
}

std::uint16_t RecolorRgb565(std::uint16_t c, double hue) {
    auto r = static_cast<std::uint8_t>(((c >> 11) * 255 + 15) / 31);
    auto g = static_cast<std::uint8_t>((((c >> 5) & 63) * 255 + 31) / 63);
    auto b = static_cast<std::uint8_t>(((c & 31) * 255 + 15) / 31);
    Colorize(r, g, b, hue);
    const unsigned r5 = (r * 31u + 127u) / 255u, g6 = (g * 63u + 127u) / 255u, b5 = (b * 31u + 127u) / 255u;
    return static_cast<std::uint16_t>(r5 << 11 | g6 << 5 | b5);
}

}  // namespace

void Colorize(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, double hue) {
    const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    if (mx == mn) return;  // a grey keeps its look
    const double v = mx / 255.0, s = (mx - mn) / static_cast<double>(mx);
    double h = std::fmod(hue, 360.0);
    if (h < 0.0) h += 360.0;
    const double c = v * s, x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0)), m = v - c;
    double rr = 0.0, gg = 0.0, bb = 0.0;
    switch (static_cast<int>(h / 60.0) % 6) {
        case 0: rr = c; gg = x; break;
        case 1: rr = x; gg = c; break;
        case 2: gg = c; bb = x; break;
        case 3: gg = x; bb = c; break;
        case 4: rr = x; bb = c; break;
        default: rr = c; bb = x; break;
    }
    r = To8(rr + m);
    g = To8(gg + m);
    b = To8(bb + m);
}

std::optional<int> RecolorEffectList(Bytes& efl, double hue) {
    if (efl.size() < kEflHeader || std::memcmp(efl.data(), "EFL\0", 4) != 0 || U32(efl.data() + 4) != kEflVersion) return std::nullopt;
    const std::size_t size = U32(efl.data() + 8), count = U32(efl.data() + 12);
    if (size > efl.size() - kEflHeader || count > size / kPartEntry) return std::nullopt;
    std::uint8_t* data = efl.data() + kEflHeader;
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = U32(data + i * kPartEntry + 4) >> 8;
        if (at != 0) starts.push_back(at);
    }
    std::sort(starts.begin(), starts.end());
    int changed = 0;
    std::vector<std::size_t> fields;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t word = U32(data + i * kPartEntry + 4);
        const std::size_t at = word >> 8;
        if (at == 0 || at < count * kPartEntry || at >= size) continue;
        const auto next = std::upper_bound(starts.begin(), starts.end(), at);
        const std::size_t end = next == starts.end() ? size : *next;
        ColourFields(word & 0xFF, fields);
        for (const std::size_t f : fields) {
            if (at + f + 4 > end) continue;
            std::uint8_t* p = data + at + f;  // B, G, R, A
            std::uint8_t r = p[2], g = p[1], b = p[0];
            Colorize(r, g, b, hue);
            if (r == p[2] && g == p[1] && b == p[0]) continue;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            ++changed;
        }
    }
    return changed;
}

int ReplaceInlinePaths(Bytes& efl, std::string_view from, std::string_view to) {
    if (from.empty() || from.size() != to.size() || efl.size() < kEflHeader || std::memcmp(efl.data(), "EFL\0", 4) != 0) return 0;
    const std::size_t size = std::min<std::size_t>(U32(efl.data() + 8), efl.size() - kEflHeader);
    std::uint8_t* data = efl.data() + kEflHeader;
    int replaced = 0;
    for (std::size_t i = 0; i + from.size() <= size;) {
        if (std::memcmp(data + i, from.data(), from.size()) == 0) {
            std::memcpy(data + i, to.data(), to.size());
            ++replaced;
            i += from.size();
        } else {
            ++i;
        }
    }
    return replaced;
}

std::optional<int> RecolorDxt5(Bytes& tex, double hue) {
    if (tex.size() < kTexMipTable + 4 || std::memcmp(tex.data(), "TEX\0", 4) != 0 || U16(tex.data() + 4) != kTexVersion ||
        std::memcmp(tex.data() + 20, "DXT5", 4) != 0) {
        return std::nullopt;
    }
    const unsigned mips = tex[8], width = U16(tex.data() + 12), height = U16(tex.data() + 14);
    if (mips == 0 || mips > 16 || width == 0 || height == 0 || kTexMipTable + 4u * mips > tex.size()) return std::nullopt;
    // Every mip where the table says, one after another to the end of the file.
    std::size_t at = U32(tex.data() + kTexMipTable);
    if (at < kTexMipTable + 4u * mips) return std::nullopt;
    const std::size_t first = at;
    for (unsigned m = 0; m < mips; ++m) {
        if (U32(tex.data() + kTexMipTable + 4u * m) != at) return std::nullopt;
        const std::size_t bw = std::max(1u, ((width >> m) + 3u) / 4u), bh = std::max(1u, ((height >> m) + 3u) / 4u);
        at += bw * bh * 16u;
    }
    if (at != tex.size()) return std::nullopt;
    int blocks = 0;
    for (std::size_t o = first; o + 16 <= tex.size(); o += 16) {
        std::uint8_t* block = tex.data() + o;  // 8 bytes of alpha, then the colours: two RGB565 endpoints, the indices
        SetU16(block + 8, RecolorRgb565(U16(block + 8), hue));
        SetU16(block + 10, RecolorRgb565(U16(block + 10), hue));
        ++blocks;
    }
    return blocks;
}

Bytes ZlibStored(const Bytes& data) {
    Bytes out;
    out.reserve(data.size() + data.size() / 65535 * 5 + 16);
    out.push_back(0x78);  // deflate, 32K window
    out.push_back(0x01);  // no preset dictionary, fastest; (0x7801 % 31 == 0)
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, data.size() - pos);
        const bool last = pos + n == data.size();
        out.push_back(last ? 1 : 0);  // BFINAL, BTYPE 00 (stored)
        const auto len = static_cast<std::uint16_t>(n);
        const auto nlen = static_cast<std::uint16_t>(~len);
        out.push_back(static_cast<std::uint8_t>(len & 0xFF));
        out.push_back(static_cast<std::uint8_t>(len >> 8));
        out.push_back(static_cast<std::uint8_t>(nlen & 0xFF));
        out.push_back(static_cast<std::uint8_t>(nlen >> 8));
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < data.size());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    const std::uint32_t adler = b << 16 | a;
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(adler >> (8 * i)));
    return out;
}

Bytes WriteArchive(const std::vector<ArcEntry>& entries) {
    const std::size_t table = 8 + entries.size() * kArcEntry;
    std::size_t start = std::max(kArcDataStart, (table + kArcDataStart - 1) / kArcDataStart * kArcDataStart);
    std::vector<Bytes> packed;
    packed.reserve(entries.size());
    for (const ArcEntry& e : entries) {
        if (e.path.size() >= kArcPath || e.data.size() > kArcSizeMask) return {};
        packed.push_back(ZlibStored(e.data));
    }
    Bytes out;
    out.insert(out.end(), {'A', 'R', 'C', 0, 7, 0});
    out.push_back(static_cast<std::uint8_t>(entries.size() & 0xFF));
    out.push_back(static_cast<std::uint8_t>(entries.size() >> 8));
    std::size_t at = start;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ArcEntry& e = entries[i];
        Bytes name(kArcPath, 0);
        std::memcpy(name.data(), e.path.data(), e.path.size());
        out.insert(out.end(), name.begin(), name.end());
        PutU32(out, e.type);
        PutU32(out, static_cast<std::uint32_t>(packed[i].size()));
        PutU32(out, kArcSizeFlag | static_cast<std::uint32_t>(e.data.size()));
        PutU32(out, static_cast<std::uint32_t>(at));
        at += packed[i].size();
    }
    out.resize(start, 0);
    for (const Bytes& p : packed) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::optional<Bytes> ReadArchiveEntry(const Bytes& archive, std::string_view path, std::uint32_t type) {
    if (archive.size() < 8 || std::memcmp(archive.data(), "ARC\0", 4) != 0) return std::nullopt;
    const std::size_t count = U16(archive.data() + 6);
    if (8 + count * kArcEntry > archive.size()) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* e = archive.data() + 8 + i * kArcEntry;
        const auto* name = reinterpret_cast<const char*>(e);
        const std::size_t length = strnlen(name, kArcPath);
        if (std::string_view(name, length) != path || U32(e + kArcPath) != type) continue;
        const std::size_t packedSize = U32(e + kArcPath + 4), size = U32(e + kArcPath + 8) & kArcSizeMask, at = U32(e + kArcPath + 12);
        if (at > archive.size() || packedSize > archive.size() - at) return std::nullopt;
        Bytes out;
        if (!ZlibInflate(archive.data() + at, packedSize, out, size + 1) || out.size() != size) return std::nullopt;
        return out;
    }
    return std::nullopt;
}

std::string FlameEffectPath(char code, int part) {
    std::string path = "effect\\efl\\wp\\wp21\\wp21";
    path += code;
    path += static_cast<char>('0' + part);
    return path;
}

std::optional<Bytes> BuildFlameEffects(const Bytes& wp21Arc, const Bytes& coreEffArc, std::vector<std::string>* log) {
    const auto note = [log](const std::string& line) {
        if (log != nullptr) log->push_back(line);
    };
    struct Source {
        int part;          // 0 the stream, 4 its companion, 3 the hit's look
        const char* path;
        Bytes data;
    };
    std::vector<Source> lists = {{0, "effect\\efl\\wp\\wp21\\wp2100", {}}, {4, "effect\\efl\\wp\\wp21\\wp2104", {}},
                                 {3, "effect\\efl\\wp\\wp21\\wp2103", {}}};
    for (Source& s : lists) {
        auto data = ReadArchiveEntry(wp21Arc, s.path, kTypeEffectList);
        if (!data) {
            note(std::string("not in uWp21.arc: ") + s.path);
            return std::nullopt;
        }
        s.data = std::move(*data);
    }
    // The flame's coloured textures (the grey ones take a list's colours as they are).
    const char* textures[] = {"0003", "0006", "0007", "0008"};
    struct Texture {
        std::string number;
        Bytes image, frames;
    };
    std::vector<Texture> images;
    for (const char* n : textures) {
        const std::string folder = std::string("effect\\tex\\") + n + "\\tx" + n;
        auto image = ReadArchiveEntry(coreEffArc, folder + "_BM", kTypeTexture);
        auto frames = ReadArchiveEntry(coreEffArc, folder, kTypeEffectAnim);
        if (!image || !frames) {
            note("not in coreEff.arc: " + folder + (image ? "" : "_BM"));
            return std::nullopt;
        }
        images.push_back({n, std::move(*image), std::move(*frames)});
    }
    std::vector<ArcEntry> entries;
    std::vector<ArcEntry> effectLists;  // after the textures they name
    for (const FlameLook& look : kFlameLooks) {
        std::vector<std::pair<std::string, std::string>> renames;
        if (look.stream) {
            for (const Texture& t : images) {
                const std::string folder = std::string(1, look.code) + t.number.substr(1);  // "c006"
                Bytes image = t.image;
                const auto blocks = RecolorDxt5(image, look.hue);
                if (!blocks) {
                    note("tx" + t.number + "_BM is not a DXT5 texture this reads");
                    return std::nullopt;
                }
                entries.push_back({"effect\\tex\\" + folder + "\\tx" + t.number + "_BM", kTypeTexture, std::move(image)});
                entries.push_back({"effect\\tex\\" + folder + "\\tx" + t.number, kTypeEffectAnim, t.frames});
                renames.emplace_back("effect\\tex\\" + t.number + "\\", "effect\\tex\\" + folder + "\\");
                note("tx" + t.number + "_BM -> " + folder + ": " + std::to_string(*blocks) + " blocks");
            }
        }
        for (const Source& s : lists) {
            if (!look.stream && s.part != 3) continue;
            Bytes list = s.data;
            const auto colours = RecolorEffectList(list, look.hue);
            if (!colours) {
                note(std::string(s.path) + " is not an effect list this reads");
                return std::nullopt;
            }
            int renamed = 0;
            for (const auto& [from, to] : renames) renamed += ReplaceInlinePaths(list, from, to);
            const std::string path = FlameEffectPath(look.code, s.part);
            note(path + " (" + look.element + "): " + std::to_string(*colours) + " colours, " + std::to_string(renamed) + " texture paths");
            effectLists.push_back({path, kTypeEffectList, std::move(list)});
        }
    }
    // Plague's death cloud: the incendiary grenade's fire in the toxic look's green, every texture it
    // names recoloured under "effect\tex\g<nnn>" (a texture this cannot recolour keeps its own).
    if (auto cloud = ReadArchiveEntry(coreEffArc, kMiasmaSource, kTypeEffectList)) {
        double hue = 105.0;
        for (const FlameLook& look : kFlameLooks) {
            if (std::string_view(look.element) == "toxic") hue = look.hue;
        }
        std::vector<std::string> numbers;  // the textures it names: "effect\tex\<nnnn>\"
        const std::string_view text(reinterpret_cast<const char*>(cloud->data()), cloud->size());
        for (std::size_t at = text.find("effect\\tex\\"); at != std::string_view::npos; at = text.find("effect\\tex\\", at + 1)) {
            if (at + 16 > text.size() || text[at + 15] != '\\') continue;
            const std::string n(text.substr(at + 11, 4));
            if (std::find(numbers.begin(), numbers.end(), n) == numbers.end()) numbers.push_back(n);
        }
        std::vector<std::pair<std::string, std::string>> renames;
        for (const std::string& n : numbers) {
            const std::string from = "effect\\tex\\" + n + "\\tx" + n;
            auto image = ReadArchiveEntry(coreEffArc, from + "_BM", kTypeTexture);
            auto frames = ReadArchiveEntry(coreEffArc, from, kTypeEffectAnim);
            if (!image || !frames || !RecolorDxt5(*image, hue)) continue;
            const std::string folder = std::string(1, kMiasmaCode) + n.substr(1);  // "g003"
            entries.push_back({"effect\\tex\\" + folder + "\\tx" + n + "_BM", kTypeTexture, std::move(*image)});
            entries.push_back({"effect\\tex\\" + folder + "\\tx" + n, kTypeEffectAnim, std::move(*frames)});
            renames.emplace_back("effect\\tex\\" + n + "\\", "effect\\tex\\" + folder + "\\");
        }
        const auto colours = RecolorEffectList(*cloud, hue);
        if (colours) {
            int renamed = 0;
            for (const auto& [from, to] : renames) renamed += ReplaceInlinePaths(*cloud, from, to);
            const std::string path = FlameEffectPath(kMiasmaCode, 0);
            note(path + " (Plague's cloud, from " + kMiasmaSource + "): " + std::to_string(*colours) + " colours, " +
                 std::to_string(renames.size()) + " of " + std::to_string(numbers.size()) + " textures recoloured, " +
                 std::to_string(renamed) + " texture paths");
            effectLists.push_back({path, kTypeEffectList, std::move(*cloud)});
        } else {
            note(std::string(kMiasmaSource) + " is not an effect list this reads; Plague's cloud keeps its look");
        }
    } else {
        note(std::string("not in coreEff.arc: ") + kMiasmaSource + "; Plague's cloud keeps its look");
    }
    for (ArcEntry& e : effectLists) entries.push_back(std::move(e));
    Bytes archive = WriteArchive(entries);
    if (archive.empty()) {
        note("the archive could not be written");
        return std::nullopt;
    }
    note(std::to_string(entries.size()) + " entries, " + std::to_string(archive.size()) + " bytes");
    return archive;
}

}  // namespace rm::fx
