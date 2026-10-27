#include "rm/capabilities.h"

#include <algorithm>

namespace rm {

bool IsKnownCapability(std::string_view name) {
    return std::find(cap::kAll.begin(), cap::kAll.end(), name) != cap::kAll.end();
}

Capabilities::Capabilities(std::initializer_list<std::string_view> names) {
    for (std::string_view n : names) Add(n);
}

Capabilities Capabilities::All() {
    Capabilities caps;
    for (std::string_view n : cap::kAll) caps.Add(n);
    return caps;
}

void Capabilities::Add(std::string_view name) { names_.emplace(name); }

void Capabilities::Remove(std::string_view name) {
    const auto it = names_.find(name);
    if (it != names_.end()) names_.erase(it);
}

bool Capabilities::Has(std::string_view name) const { return names_.find(name) != names_.end(); }

std::string Capabilities::ToString() const {
    std::string out;
    for (const std::string& n : names_) {
        if (!out.empty()) out += ',';
        out += n;
    }
    return out;
}

}  // namespace rm
