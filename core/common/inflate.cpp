#include "rm/inflate.h"

#include <array>
#include <utility>

namespace rm {
namespace {

// Reads the stream least significant bit first, one byte at a time as bits are needed.
struct BitReader {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    std::uint32_t bits = 0;
    int count = 0;
    bool overrun = false;

    std::uint32_t Get(int n) {
        while (count < n) {
            if (pos >= size) {
                overrun = true;
                return 0;
            }
            bits |= static_cast<std::uint32_t>(data[pos++]) << count;
            count += 8;
        }
        const std::uint32_t value = n == 0 ? 0u : bits & ((1u << n) - 1u);
        bits = n == 0 ? bits : bits >> n;
        count -= n;
        return value;
    }
    // Stored blocks and the trailer start on a byte boundary.
    void Align() { Get(count % 8); }
};

// A canonical Huffman code: how many codes of each length, and the symbols in code order.
struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 288> symbol{};
};

// False when the lengths over-subscribe the code. Incomplete codes are accepted; a code that is
// never assigned simply fails to decode.
bool Build(Huffman& h, const std::uint8_t* lengths, int n) {
    h.count.fill(0);
    for (int s = 0; s < n; ++s) ++h.count[lengths[s]];
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left = (left << 1) - h.count[len];
        if (left < 0) return false;
    }
    std::array<std::uint16_t, 16> offset{};
    for (int len = 1; len < 15; ++len) offset[len + 1] = static_cast<std::uint16_t>(offset[len] + h.count[len]);
    for (int s = 0; s < n; ++s) {
        if (lengths[s] != 0) h.symbol[offset[lengths[s]]++] = static_cast<std::uint16_t>(s);
    }
    return true;
}

// One symbol, a bit at a time: codes of each length are consecutive integers.
int Decode(BitReader& in, const Huffman& h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(in.Get(1));
        if (in.overrun) return -1;
        const int count = h.count[len];
        if (code - first < count) return h.symbol[static_cast<std::size_t>(index + code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                         33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                         1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(BitReader& in, std::vector<std::uint8_t>& out, const Huffman& literals, const Huffman& distances, std::size_t limit) {
    for (;;) {
        int symbol = Decode(in, literals);
        if (symbol < 0) return false;
        if (symbol < 256) {
            out.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            const std::size_t length = kLengthBase[static_cast<std::size_t>(symbol)] + in.Get(kLengthExtra[static_cast<std::size_t>(symbol)]);
            const int d = Decode(in, distances);
            if (d < 0 || d >= 30) return false;
            const std::size_t back = kDistanceBase[static_cast<std::size_t>(d)] + in.Get(kDistanceExtra[static_cast<std::size_t>(d)]);
            if (in.overrun || back > out.size()) return false;
            for (std::size_t i = 0; i < length; ++i) {
                const std::uint8_t byte = out[out.size() - back];
                out.push_back(byte);
            }
        }
        if (limit != 0 && out.size() > limit) return false;
    }
}

bool Stored(BitReader& in, std::vector<std::uint8_t>& out, std::size_t limit) {
    in.Align();
    const std::uint32_t length = in.Get(16);
    const std::uint32_t inverse = in.Get(16);
    if (in.overrun || (length ^ 0xFFFFu) != inverse) return false;
    for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t byte = in.Get(8);
        if (in.overrun) return false;
        out.push_back(static_cast<std::uint8_t>(byte));
    }
    return limit == 0 || out.size() <= limit;
}

bool Fixed(BitReader& in, std::vector<std::uint8_t>& out, std::size_t limit) {
    static const auto tables = [] {
        std::array<std::uint8_t, 288> lengths{};
        for (int s = 0; s < 288; ++s) lengths[static_cast<std::size_t>(s)] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
        std::array<std::uint8_t, 30> distances{};
        distances.fill(5);
        std::pair<Huffman, Huffman> t;
        Build(t.first, lengths.data(), 288);
        Build(t.second, distances.data(), 30);
        return t;
    }();
    return Codes(in, out, tables.first, tables.second, limit);
}

bool Dynamic(BitReader& in, std::vector<std::uint8_t>& out, std::size_t limit) {
    static constexpr std::array<std::uint8_t, 19> kOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int literalCount = static_cast<int>(in.Get(5)) + 257;
    const int distanceCount = static_cast<int>(in.Get(5)) + 1;
    const int lengthCount = static_cast<int>(in.Get(4)) + 4;
    if (in.overrun || literalCount > 286 || distanceCount > 30) return false;
    std::array<std::uint8_t, 320> lengths{};
    for (int i = 0; i < lengthCount; ++i) lengths[kOrder[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(in.Get(3));
    Huffman lengthCode;
    if (in.overrun || !Build(lengthCode, lengths.data(), 19)) return false;
    lengths.fill(0);
    for (int i = 0; i < literalCount + distanceCount;) {
        const int symbol = Decode(in, lengthCode);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[static_cast<std::size_t>(i++)] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t value = 0;
        int repeat = 0;
        if (symbol == 16) {
            if (i == 0) return false;
            value = lengths[static_cast<std::size_t>(i - 1)];
            repeat = 3 + static_cast<int>(in.Get(2));
        } else if (symbol == 17) {
            repeat = 3 + static_cast<int>(in.Get(3));
        } else {
            repeat = 11 + static_cast<int>(in.Get(7));
        }
        if (in.overrun || i + repeat > literalCount + distanceCount) return false;
        while (repeat-- > 0) lengths[static_cast<std::size_t>(i++)] = value;
    }
    if (lengths[256] == 0) return false;  // no end-of-block code
    Huffman literals;
    Huffman distances;
    if (!Build(literals, lengths.data(), literalCount) || !Build(distances, lengths.data() + literalCount, distanceCount)) return false;
    return Codes(in, out, literals, distances, limit);
}

std::uint32_t Adler32(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : bytes) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

}  // namespace

bool ZlibInflate(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::size_t limit) {
    out.clear();
    if (data == nullptr || size < 6) return false;
    const unsigned cmf = data[0];
    const unsigned flg = data[1];
    if ((cmf & 0x0Fu) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20u) != 0) return false;
    BitReader in{data + 2, size - 2};
    for (bool last = false; !last;) {
        last = in.Get(1) != 0;
        const std::uint32_t type = in.Get(2);
        if (in.overrun) return false;
        const bool ok = type == 0 ? Stored(in, out, limit) : type == 1 ? Fixed(in, out, limit) : type == 2 ? Dynamic(in, out, limit) : false;
        if (!ok) return false;
    }
    in.Align();
    std::uint32_t expected = 0;
    for (int i = 0; i < 4; ++i) expected = (expected << 8) | in.Get(8);
    return !in.overrun && expected == Adler32(out);
}

}  // namespace rm
