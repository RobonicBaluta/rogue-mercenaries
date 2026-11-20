#include "rm/map_edits.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>

#include <nlohmann/json.hpp>

namespace rm {

namespace {

using nlohmann::json;

double DistanceSq(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

bool ReadPoint(const json& value, std::array<double, 3>& out) {
    if (!value.is_array() || value.size() != 3) return false;
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value[i].is_number()) return false;
        out[i] = value[i].get<double>();
    }
    return true;
}

bool ReadPoints(const json& value, std::vector<std::array<double, 3>>& out) {
    if (!value.is_array()) return false;
    for (const json& p : value) {
        std::array<double, 3> point{};
        if (!ReadPoint(p, point)) return false;
        out.push_back(point);
    }
    return true;
}

json WritePoint(const std::array<double, 3>& p) {
    // A tenth of an engine unit is far finer than a spawn needs; keeps the file readable.
    json out = json::array();
    for (const double v : p) out.push_back(std::round(v * 10.0) / 10.0);
    return out;
}

}  // namespace

bool RemovesPoint(const MapEdit& edit, const std::array<double, 3>& p) {
    constexpr double kRadiusSq = kMapEditMatchRadius * kMapEditMatchRadius;
    return std::any_of(edit.removed.begin(), edit.removed.end(), [&](const auto& r) { return DistanceSq(r, p) <= kRadiusSq; });
}

MapDef ApplyMapEdit(const MapDef& map, const MapEdit& edit) {
    MapDef out = map;
    std::vector<std::array<double, 3>> points;
    for (const auto& p : map.points) {
        if (!edit.clear && !RemovesPoint(edit, p)) points.push_back(p);
    }
    points.insert(points.end(), edit.added.begin(), edit.added.end());
    if (!points.empty()) out.points = std::move(points);
    if (edit.start) out.start = edit.start;
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::erase_if(out.hide_models, [&](const std::string& name) {
        return std::any_of(edit.shown.begin(), edit.shown.end(), [&](const std::string& s) { return lower(s) == lower(name); });
    });
    for (const std::string& name : edit.hidden) {
        if (std::none_of(out.hide_models.begin(), out.hide_models.end(), [&](const std::string& h) { return lower(h) == lower(name); })) {
            out.hide_models.push_back(name);
        }
    }
    return out;
}

std::optional<MapEdits> ParseMapEdits(std::string_view text, std::string* error) {
    auto fail = [error](const std::string& why) -> std::optional<MapEdits> {
        if (error) *error = why;
        return std::nullopt;
    };
    json doc;
    try {
        doc = json::parse(text.begin(), text.end());
    } catch (const std::exception& e) {
        return fail(std::string("not JSON: ") + e.what());
    }
    if (!doc.is_object()) return fail("not a JSON object");
    MapEdits edits;
    const auto maps = doc.find("maps");
    if (maps == doc.end()) return edits;
    if (!maps->is_object()) return fail("'maps' must be an object of map key -> edit");
    for (const auto& [key, value] : maps->items()) {
        char* end = nullptr;
        const long stage = std::strtol(key.c_str(), &end, 10);
        if (key.empty() || end == nullptr || *end != '\0') return fail("map key '" + key + "' is not a number");
        if (!value.is_object()) return fail("map " + key + ": not an object");
        MapEdit edit;
        if (const auto a = value.find("added"); a != value.end() && !ReadPoints(*a, edit.added)) {
            return fail("map " + key + ": 'added' must be a list of [x, y, z]");
        }
        if (const auto r = value.find("removed"); r != value.end() && !ReadPoints(*r, edit.removed)) {
            return fail("map " + key + ": 'removed' must be a list of [x, y, z]");
        }
        for (const char* field : {"hidden", "shown"}) {
            const auto list = value.find(field);
            if (list == value.end()) continue;
            if (!list->is_array()) return fail("map " + key + ": '" + field + "' must be a list of names");
            for (const json& n : *list) {
                if (!n.is_string() || n.get<std::string>().empty()) return fail("map " + key + ": '" + field + "' must be a list of names");
                (std::string_view(field) == "hidden" ? edit.hidden : edit.shown).push_back(n.get<std::string>());
            }
        }
        if (const auto c = value.find("clear"); c != value.end()) {
            if (!c->is_boolean()) return fail("map " + key + ": 'clear' must be true or false");
            edit.clear = c->get<bool>();
        }
        if (const auto s = value.find("start"); s != value.end() && !s->is_null()) {
            std::array<double, 3> start{};
            if (!ReadPoint(*s, start)) return fail("map " + key + ": 'start' must be [x, y, z]");
            edit.start = start;
        }
        if (!edit.empty()) edits[static_cast<int>(stage)] = std::move(edit);
    }
    return edits;
}

std::string SerializeMapEdits(const MapEdits& edits) {
    json maps = json::object();
    for (const auto& [stage, edit] : edits) {
        if (edit.empty()) continue;
        json e = json::object();
        if (!edit.added.empty()) {
            json list = json::array();
            for (const auto& p : edit.added) list.push_back(WritePoint(p));
            e["added"] = std::move(list);
        }
        if (!edit.removed.empty()) {
            json list = json::array();
            for (const auto& p : edit.removed) list.push_back(WritePoint(p));
            e["removed"] = std::move(list);
        }
        if (edit.clear) e["clear"] = true;
        if (edit.start) e["start"] = WritePoint(*edit.start);
        if (!edit.hidden.empty()) e["hidden"] = edit.hidden;
        if (!edit.shown.empty()) e["shown"] = edit.shown;
        maps[std::to_string(stage)] = std::move(e);
    }
    json doc = json::object();
    doc["schema_version"] = 1;
    doc["maps"] = std::move(maps);
    return doc.dump(2) + "\n";
}

}  // namespace rm
