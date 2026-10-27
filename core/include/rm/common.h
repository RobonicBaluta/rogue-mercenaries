#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>

namespace rm {

/// Ordered containers with heterogeneous lookup, so std::string_view keys work without copies.
/// Ordered (not hashed) so iteration order is deterministic across builds and architectures.
using StringSet = std::set<std::string, std::less<>>;

template <class V>
using StringMap = std::map<std::string, V, std::less<>>;

}  // namespace rm
