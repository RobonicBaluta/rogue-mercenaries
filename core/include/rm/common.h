#pragma once

#include <filesystem>
#include <string>

#include <functional>
#include <map>
#include <set>
#include <string>

namespace rm {

/// A path as UTF-8 text for logs and messages. path::string() converts to the system code page and
/// throws on a character it cannot hold (a Windows user name such as "\u0218tefan"), which took the
/// whole game down at startup.
inline std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}


/// Ordered containers with heterogeneous lookup, so std::string_view keys work without copies.
/// Ordered (not hashed) so iteration order is deterministic across builds and architectures.
using StringSet = std::set<std::string, std::less<>>;

template <class V>
using StringMap = std::map<std::string, V, std::less<>>;

}  // namespace rm
