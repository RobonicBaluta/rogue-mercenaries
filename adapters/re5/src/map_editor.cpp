#include "map_editor.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>

#include "director.h"
#include "engine.h"
#include "hooks.h"
#include "log.h"
#include "memory.h"
#include "overlay.h"
#include "re5_layout.h"
#include "resources.h"
#include "world.h"
#include "rm/common.h"
#include "rm/content.h"
#include "rm/map_edits.h"
#include "rm/run_controller.h"
#include "rm/screen_fx.h"

namespace rmre5::mapeditor {
namespace {

using Point = std::array<double, 3>;

constexpr double kNearReach = 1500.0;     // engine units (15 m): "the nearest point" to the player
constexpr double kLaserPick = 500.0;      // a point or a taken-out point within 5 m of the laser
constexpr float kLaserReach = 6000.0f;    // the laser's ray: 60 m from the camera
constexpr float kGroundProbe = 1200.0f;   // the ground under the laser: up to 12 m below its hit
constexpr double kMarkerReach = 8000.0;   // points farther than 80 m are not drawn
constexpr float kMarkerLift = 60.0f;      // drawn a little above the ground they stand on
constexpr int kListRows = 12;
constexpr float kObjectReach = 1000.0f;       // objects listed within 10 m of the player
constexpr float kLaserObjectReach = 600.0f;   // and within 6 m of the laser (a door's record is at its hinge)
constexpr std::size_t kObjectRows = 10;
constexpr unsigned short kPadCombo = 0x0040 | 0x0080;  // XInput left and right stick clicks (L3 + R3)
constexpr double kReached = 300.0;        // a test enemy within 3 m of the player has reached him
constexpr double kTooClose = 400.0;       // a point within 4 m of the player is not worth a test
constexpr double kTestBase = 20.0;        // seconds a test enemy gets, plus one per 1.5 m away, up to 90
constexpr double kTestPerMetre = 1.0 / 1.5;
constexpr double kTestMax = 90.0;
constexpr double kKeepGoingPause = 1.0;   // seconds between one result and the next test

std::atomic<bool> g_enabled{false};
std::atomic<int> g_serial{0};
std::atomic<bool> g_invulnerable{false};
std::atomic<bool> g_holdWaves{false};
bool g_editorRunPending = false;  // MAP EDITOR started a run: invulnerable once it is on
bool g_keepGoing = false;
bool g_open = false;
bool g_justOpened = false;
bool g_used = false;  // opened once this session: the points show in the world from then on
bool g_showMarkers = true;
std::filesystem::path g_file;
std::string g_status;
int g_stepIndex = -1;  // "next point": the point the player was sent to last
int g_testClass = 0;
int g_travelPick = -1;     // the map list's choice (a map key)
int g_travelRequest = -1;  // asked for, for the game tick (TakeTravelRequest)
unsigned short g_prevPad = 0;

// Where the middle of the screen (the laser while aiming) meets the world: the ray's hit and the
// ground under it (a hit on a wall is no place to stand). Worked out on the game tick.
struct Laser {
    Point hit{};
    Point ground{};
    double distance = 0.0;  // from the player
};
std::optional<Laser> g_laser;
std::optional<Laser> g_laserAtOpen;  // held while the window is up

struct Pending {
    enum class Kind { None, Warp, Spawn } kind = Kind::None;
    float pos[3]{};
    std::string unitClass;
};
std::vector<Pending> g_pending;

double DistanceSq(const Point& a, const Point& b);

// Testing points one at a time: a test enemy at one point; did it reach the player, how fast?
struct TestResult {
    Point pos{};
    int index = 0;
    enum class Kind { Reached, NotReached, Lost, NoSpawn } kind = Kind::NotReached;
    double seconds = 0.0;
    double closest = 0.0;  // how near it came (engine units)
};
struct PointTest {
    bool active = false;
    int mapKey = -1;
    Point pos{};
    int index = 0;
    std::string unitClass;
    std::uintptr_t unit = 0;
    double started = 0.0, limit = 0.0, closest = 0.0, distance = 0.0;
};
PointTest g_test;
struct TestRequest {
    int mapKey = -1;
    Point pos{};
    int index = 0;
    std::string unitClass;
};
std::optional<TestRequest> g_testRequest;
bool g_stopTest = false;
double g_nextAutoTest = 0.0;  // keep going: when the next test starts (0: none waiting)
std::map<int, std::vector<TestResult>> g_results;  // by map key, this game session
std::string g_testLine;  // the HUD line while (and just after) a test runs
double g_testLineUntil = 0.0;

double Seconds() { return static_cast<double>(GetTickCount64()) / 1000.0; }

const TestResult* ResultAt(int mapKey, const Point& p) {
    const auto it = g_results.find(mapKey);
    if (it == g_results.end()) return nullptr;
    for (const TestResult& r : it->second) {
        if (DistanceSq(r.pos, p) <= rm::kMapEditMatchRadius * rm::kMapEditMatchRadius) return &r;
    }
    return nullptr;
}

void Record(int mapKey, TestResult result) {
    auto& list = g_results[mapKey];
    std::erase_if(list, [&](const TestResult& r) { return DistanceSq(r.pos, result.pos) <= rm::kMapEditMatchRadius * rm::kMapEditMatchRadius; });
    list.push_back(result);
}

double DistanceSq(const Point& a, const Point& b) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

std::optional<Point> PlayerPosition() {
    const auto local = engine::LocalPlayer();
    if (!local) return std::nullopt;
    return Point{ReadOr<float>(local + re5::off::kUnitPosition, 0.0f), ReadOr<float>(local + re5::off::kUnitPosition + 4, 0.0f),
                 ReadOr<float>(local + re5::off::kUnitPosition + 8, 0.0f)};
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool Save(const rm::MapEdits& edits) {
    if (g_file.empty()) return false;
    std::error_code ec;
    const auto temp = std::filesystem::path(g_file).replace_extension(L".json.tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << rm::SerializeMapEdits(edits);
        if (!out) return false;
    }
    std::filesystem::rename(temp, g_file, ec);
    return !ec;
}

// The edit applied: to the run (the arena on that map takes it at once) and to the file.
void Commit(rm::RunController& run, rm::MapEdits edits, int mapKey, const std::string& what, bool loads = false) {
    run.SetMapEdits(edits);
    if (loads) g_serial.fetch_add(1);  // objects: the next load's settings are worked out again
    const bool saved = Save(edits);
    const auto it = edits.find(mapKey);
    const std::size_t added = it != edits.end() ? it->second.added.size() : 0;
    const std::size_t removed = it != edits.end() ? it->second.removed.size() : 0;
    RM_LOGI("map editor: %s on map %d (%zu added, %zu taken out%s%s)%s", what.c_str(), mapKey, added, removed,
            it != edits.end() && it->second.start ? ", own start" : "", it != edits.end() && it->second.start2 ? ", player 2's start" : "",
            saved ? "" : "; map_edits.json NOT written");
    g_status = what + (saved ? "." : ". Could not write map_edits.json!");
}

struct Marked {
    Point pos;
    bool added = false;  // the editor's own (else the map's)
    double distance = 0.0;
    int index = 0;       // in the arena's points
};

// The arena's points (the map with the edits and the run's guests), each marked and measured from `from`.
std::vector<Marked> Points(rm::RunController& run, int mapKey, const std::optional<Point>& from) {
    std::vector<Marked> out;
    const std::optional<rm::MapDef> map = run.ArenaMap(mapKey);
    if (!map) return out;
    const auto edit = run.map_edits().find(mapKey);
    for (std::size_t i = 0; i < map->points.size(); ++i) {
        Marked m;
        m.pos = map->points[i];
        m.index = static_cast<int>(i);
        m.added = edit != run.map_edits().end() &&
                  std::find(edit->second.added.begin(), edit->second.added.end(), m.pos) != edit->second.added.end();
        m.distance = from ? std::sqrt(DistanceSq(m.pos, *from)) : 0.0;
        out.push_back(m);
    }
    return out;
}

const Marked* Nearest(const std::vector<Marked>& points, double reach) {
    const Marked* best = nullptr;
    for (const Marked& m : points) {
        if (m.distance <= reach && (best == nullptr || m.distance < best->distance)) best = &m;
    }
    return best;
}

const Point* NearestRemoved(const rm::MapEdit& edit, const Point& from, double reach) {
    const Point* best = nullptr;
    double bestDistance = reach;
    for (const Point& p : edit.removed) {
        const double d = std::sqrt(DistanceSq(p, from));
        if (d <= bestDistance) {
            bestDistance = d;
            best = &p;
        }
    }
    return best;
}

// The classes a test enemy can be: the arena's roster whose archives this stage loaded.
std::vector<std::string> TestClasses(rm::RunController& run, int mapKey) {
    std::vector<std::string> out;
    const std::optional<rm::MapDef> map = run.ArenaMap(mapKey);
    auto add = [&](const std::string& cls) {
        if (cls.empty() || std::find(out.begin(), out.end(), cls) != out.end()) return;
        if (resources::Available() && !resources::ArchiveLoaded(cls)) return;
        out.push_back(cls);
    };
    if (map) {
        for (const rm::MapEnemy& e : map->roster) {
            if (!e.special) add(e.unit_class);
        }
        for (const rm::MapEnemy& e : map->roster) add(e.unit_class);
    }
    add("uEm10");
    return out;
}

void Queue(Pending::Kind kind, const Point& at, std::string cls = {}) {
    Pending p;
    p.kind = kind;
    for (int i = 0; i < 3; ++i) p.pos[i] = static_cast<float>(at[static_cast<std::size_t>(i)]);
    p.unitClass = std::move(cls);
    g_pending.push_back(std::move(p));
}

// The camera's middle ray against the world (engine::CastSegment, the game's own query).
void UpdateLaser() {
    g_laser.reset();
    const auto views = engine::ReadCameraViews();
    if (views.empty()) return;
    const engine::CameraView& cam = views.front();
    float dir[3] = {cam.target[0] - cam.eye[0], cam.target[1] - cam.eye[1], cam.target[2] - cam.eye[2]};
    const float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (length < 1.0f) return;
    for (float& d : dir) d /= length;
    const float to[3] = {cam.eye[0] + dir[0] * kLaserReach, cam.eye[1] + dir[1] * kLaserReach, cam.eye[2] + dir[2] * kLaserReach};
    const auto hit = engine::CastSegment(cam.eye, to);
    if (!hit || hit->flags == 0) return;
    Laser laser;
    laser.hit = {hit->hit[0], hit->hit[1], hit->hit[2]};
    laser.ground = laser.hit;
    // The ground under the hit, from a little before it (toward the camera) and above.
    const float above[3] = {hit->hit[0] - dir[0] * 30.0f, hit->hit[1] + 120.0f, hit->hit[2] - dir[2] * 30.0f};
    const float below[3] = {above[0], above[1] - kGroundProbe, above[2]};
    if (const auto ground = engine::CastSegment(above, below); ground && ground->flags != 0) {
        laser.ground = {ground->hit[0], ground->hit[1], ground->hit[2]};
    }
    const std::optional<Point> player = PlayerPosition();
    laser.distance = player ? std::sqrt(DistanceSq(laser.hit, *player)) : 0.0;
    g_laser = laser;
}

struct Projector {
    rm::screenfx::Mat4 viewProj{};
    float x0 = 0, y0 = 0, w = 0, h = 0;
    std::optional<Point> player;
    std::optional<rm::screenfx::ScreenPoint> operator()(const Point& p, float lift = kMarkerLift) const {
        if (player && DistanceSq(p, *player) > kMarkerReach * kMarkerReach) return std::nullopt;
        const float at[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]) + lift, static_cast<float>(p[2])};
        auto s = rm::screenfx::Project(viewProj, at, w, h);
        if (s) {
            s->x += x0;
            s->y += y0;
        }
        return s;
    }
};

float MarkerSize(const rm::screenfx::ScreenPoint& s) { return std::clamp(2500.0f / std::max(s.depth, 1.0f), 4.0f, 16.0f); }

void DrawMarkers(rm::RunController& run, int mapKey, const std::optional<Point>& player) {
    const auto views = engine::ReadCameraViews();
    if (views.empty()) return;
    const std::vector<Marked> points = Points(run, mapKey, player);
    const auto edit = run.map_edits().find(mapKey);
    const std::optional<rm::MapDef> map = run.ArenaMap(mapKey);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* list = ImGui::GetBackgroundDrawList();
    for (const engine::CameraView& cam : views) {
        const bool region = cam.region[2] > cam.region[0] && cam.region[3] > cam.region[1];
        if (views.size() > 1 && !region) continue;
        Projector project;
        project.x0 = region ? static_cast<float>(cam.region[0]) : 0.0f;
        project.y0 = region ? static_cast<float>(cam.region[1]) : 0.0f;
        project.w = region ? static_cast<float>(cam.region[2] - cam.region[0]) : display.x;
        project.h = region ? static_cast<float>(cam.region[3] - cam.region[1]) : display.y;
        project.player = player;
        project.viewProj = rm::screenfx::Multiply(rm::screenfx::LookAtRH(cam.eye, cam.target, cam.up),
                                                  rm::screenfx::PerspectiveFovRH(cam.fov, cam.aspect, cam.nearPlane, cam.farPlane));
        list->PushClipRect(ImVec2(project.x0, project.y0), ImVec2(project.x0 + project.w, project.y0 + project.h), true);
        for (const Marked& m : points) {
            const auto s = project(m.pos);
            if (!s) continue;
            const float r = MarkerSize(*s);
            const ImU32 colour = m.added ? IM_COL32(90, 235, 120, 230) : IM_COL32(80, 200, 255, 230);
            list->AddCircleFilled(ImVec2(s->x, s->y), r, colour);
            list->AddCircle(ImVec2(s->x, s->y), r, IM_COL32(0, 0, 0, 200), 0, 1.5f);
            if (const TestResult* t = ResultAt(mapKey, m.pos)) {
                const ImU32 ring = t->kind == TestResult::Kind::Reached ? IM_COL32(60, 255, 90, 255)
                                   : t->kind == TestResult::Kind::NotReached ? IM_COL32(255, 50, 40, 255)
                                                                             : IM_COL32(200, 200, 200, 200);
                list->AddCircle(ImVec2(s->x, s->y), r + 4.0f, ring, 0, 3.0f);
            }
            if (g_test.active && g_test.mapKey == mapKey && DistanceSq(g_test.pos, m.pos) < 1.0) {
                list->AddCircle(ImVec2(s->x, s->y), r + 8.0f, IM_COL32(255, 230, 60, 255), 0, 2.0f);
            }
            char label[8];
            std::snprintf(label, sizeof(label), "%d", m.index + 1);
            list->AddText(ImVec2(s->x + r + 2.0f, s->y - 7.0f), IM_COL32(255, 255, 255, 230), label);
        }
        if (edit != run.map_edits().end()) {
            for (const Point& p : edit->second.removed) {
                const auto s = project(p);
                if (!s) continue;
                const float r = MarkerSize(*s);
                list->AddLine(ImVec2(s->x - r, s->y - r), ImVec2(s->x + r, s->y + r), IM_COL32(240, 70, 60, 230), 3.0f);
                list->AddLine(ImVec2(s->x - r, s->y + r), ImVec2(s->x + r, s->y - r), IM_COL32(240, 70, 60, 230), 3.0f);
            }
        }
        // The starts: player 1's yellow, player 2's orange.
        const auto startMarker = [&](const std::optional<Point>& at, ImU32 fill, ImU32 text, const char* label) {
            if (!at) return;
            if (const auto s = project(*at)) {
                const float r = MarkerSize(*s) + 3.0f;
                const ImVec2 c(s->x, s->y);
                list->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), fill);
                list->AddText(ImVec2(c.x + r + 2.0f, c.y - 7.0f), text, label);
            }
        };
        if (map) {
            startMarker(map->start, IM_COL32(255, 210, 60, 230), IM_COL32(255, 230, 120, 230), "start 1");
            startMarker(map->start2, IM_COL32(255, 140, 40, 230), IM_COL32(255, 180, 110, 230), "start 2");
        }
        // The laser: a magenta ring where it meets the world, a line down to the ground under it.
        if (const std::optional<Laser>& laser = g_open && g_laserAtOpen ? g_laserAtOpen : g_laser) {
            const auto hit = project(laser->hit, 0.0f);
            const auto ground = project(laser->ground, 0.0f);
            const ImU32 magenta = IM_COL32(255, 80, 230, 240);
            if (hit && ground && std::fabs(laser->hit[1] - laser->ground[1]) > 30.0) {
                list->AddLine(ImVec2(hit->x, hit->y), ImVec2(ground->x, ground->y), magenta, 2.0f);
            }
            if (ground) {
                const float r = MarkerSize(*ground) + 4.0f;
                list->AddCircle(ImVec2(ground->x, ground->y), r, magenta, 0, 3.0f);
                list->AddLine(ImVec2(ground->x - r * 0.5f, ground->y), ImVec2(ground->x + r * 0.5f, ground->y), magenta, 2.0f);
                list->AddLine(ImVec2(ground->x, ground->y - r * 0.5f), ImVec2(ground->x, ground->y + r * 0.5f), magenta, 2.0f);
                char text[24];
                std::snprintf(text, sizeof(text), "laser %.1f m", laser->distance / 100.0);
                list->AddText(ImVec2(ground->x + r + 3.0f, ground->y + 2.0f), magenta, text);
            }
        }
        list->PopClipRect();
    }
}

// Adds or takes out a point, in `edit` (a copy of the run's), then commits.
void TakeOut(rm::RunController& run, rm::MapEdits& edits, rm::MapEdit& edit, int mapKey, const Marked& m) {
    if (m.added) {
        std::erase(edit.added, m.pos);
    } else {
        edit.removed.push_back(m.pos);
    }
    Commit(run, edits, mapKey, "point " + std::to_string(m.index + 1) + " taken out");
}

// The area's objects around `center`: each with its name, how far, and a button to take it out
// (or put it back) from the next load. True when something changed (the window ends its frame).
bool DrawObjects(rm::RunController& run, rm::MapEdits& edits, rm::MapEdit& edit, int mapKey, const rm::MapDef* base,
                 const Point& center, float reach, const char* id) {
    const float at[3] = {static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2])};
    const std::vector<world::NearObject> objects = world::ObjectsNear(at, reach, kObjectRows);
    const std::optional<rm::MapDef> arena = run.ArenaMap(mapKey);
    auto covers = [](const std::string& entry, const std::string& name) { return Lower(name).rfind(Lower(entry), 0) == 0; };
    if (objects.empty()) ImGui::TextDisabled("No object of the area within %.0f m.", reach / 100.0f);
    for (const world::NearObject& o : objects) {
        const bool hidden = arena && std::any_of(arena->hide_models.begin(), arena->hide_models.end(),
                                                 [&](const std::string& h) { return covers(h, o.name); });
        ImGui::PushID(id);
        ImGui::PushID(o.name.c_str());
        const bool pressed = ImGui::SmallButton(hidden ? "Put back" : "Take out");
        ImGui::SameLine();
        ImGui::Text("%-24s %5.1f m%s", o.name.c_str(), o.distance / 100.0f, hidden ? "  TAKEN OUT" : "");
        ImGui::PopID();
        ImGui::PopID();
        if (!pressed) continue;
        if (hidden) {
            std::erase_if(edit.hidden, [&](const std::string& h) { return covers(h, o.name); });
            if (base != nullptr) {
                for (const std::string& h : base->hide_models) {
                    if (covers(h, o.name)) edit.shown.push_back(h);
                }
            }
        } else {
            std::erase_if(edit.shown, [&](const std::string& s) { return covers(s, o.name); });
            edit.hidden.push_back(o.name);
        }
        Commit(run, edits, mapKey, std::string(hidden ? "object " + o.name + " put back" : "object " + o.name + " taken out") +
                                       " (from the next time this map loads)",
               true);
        return true;
    }
    return false;
}

// The window's frame; returns early (after End) on any change, so the next frame shows it.
void DrawWindow(rm::RunController& run, int mapKey, const std::optional<Point>& player) {
    const rm::MapDef* base = run.content().FindMapByStage(mapKey);
    ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, 0.0f), ImVec2(600.0f, ImGui::GetIO().DisplaySize.y * 0.92f));
    if (g_justOpened) ImGui::SetNextWindowFocus();
    ImGui::Begin("MAP EDITOR", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    const bool focusFirst = g_justOpened;
    g_justOpened = false;
    // B on the pad (or Esc) closes it, unless a list is open.
    if ((ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        g_open = false;
        overlay::SetFocused(false);
        ImGui::End();
        return;
    }
    if (base == nullptr || !player) {
        ImGui::TextWrapped(base == nullptr ? "No map of the mod is being played. Start an arena (MAP EDITOR in the mod menu starts one "
                                             "on the map you choose)."
                                           : "Waiting for the player.");
        ImGui::TextDisabled("B, F8 or L3 + R3 close this window.");
        ImGui::End();
        return;
    }
    rm::MapEdits edits = run.map_edits();
    rm::MapEdit& edit = edits[mapKey];
    const std::vector<Marked> points = Points(run, mapKey, player);
    const std::size_t yours = static_cast<std::size_t>(std::count_if(points.begin(), points.end(), [](const Marked& m) { return m.added; }));
    ImGui::Text("%s (stage %d)   spawn points %zu: the map's %zu, yours %zu, taken out %s", base->name.c_str(), mapKey, points.size(),
                points.size() - yours, yours, edit.clear ? "all of the map's" : std::to_string(edit.removed.size()).c_str());
    // Every one of the map's points out at once (yours stay), or back.
    if (ImGui::Button(edit.clear ? "Bring back the map's points" : "Take out all the map's points")) {
        const bool clearing = !edit.clear;
        edit.clear = clearing;
        if (!clearing) edit.removed.clear();
        Commit(run, edits, mapKey, clearing ? "every one of the map's points taken out (yours stay)" : "the map's points back");
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    // The whole map back as it ships: two presses within 3 s.
    static double resetArmed = 0.0;
    const bool armed = resetArmed > 0.0 && Seconds() - resetArmed < 3.0;
    if (ImGui::Button(armed ? "Press again to reset this map" : "Reset this map's edits")) {
        if (armed) {
            resetArmed = 0.0;
            edits.erase(mapKey);
            g_results.erase(mapKey);
            Commit(run, edits, mapKey, "this map's edits reset (as the mod ships it)", true);
            ImGui::End();
            return;
        }
        resetArmed = Seconds();
    }
    if (edit.clear && yours == 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Add a point of yours: until then the map uses its own.");
    }
    ImGui::Separator();

    // --- the laser (where the middle of the screen pointed when the window opened) --------------
    const std::optional<Laser>& laser = g_laserAtOpen;
    if (laser) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.9f, 1.0f), "Laser: %.1f m from you, ground at %.0f %.0f %.0f", laser->distance / 100.0,
                           laser->ground[0], laser->ground[1], laser->ground[2]);
    } else {
        ImGui::TextDisabled("Laser: it met nothing within %.0f m (aim at the ground, then open the editor).", kLaserReach / 100.0f);
    }
    const std::vector<Marked> byLaser = laser ? Points(run, mapKey, laser->ground) : std::vector<Marked>{};
    const Marked* nearLaser = Nearest(byLaser, kLaserPick);
    ImGui::BeginDisabled(!laser);
    if (ImGui::Button("Add a spawn point at the laser") && laser) {
        edit.added.push_back(laser->ground);
        Commit(run, edits, mapKey, "spawn point added at the laser");
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    if (focusFirst) ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    ImGui::BeginDisabled(nearLaser == nullptr || points.size() <= 1);
    if (ImGui::Button("Take out the point at the laser") && nearLaser != nullptr) {
        TakeOut(run, edits, edit, mapKey, *nearLaser);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    ImGui::EndDisabled();
    const Point* removedAtLaser = laser ? NearestRemoved(edit, laser->ground, kLaserPick) : nullptr;
    ImGui::BeginDisabled(removedAtLaser == nullptr);
    if (ImGui::Button("Put back the point at the laser") && removedAtLaser != nullptr) {
        const Point p = *removedAtLaser;
        std::erase(edit.removed, p);
        Commit(run, edits, mapKey, "point put back");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    ImGui::EndDisabled();
    const std::vector<std::string> classes = TestClasses(run, mapKey);
    if (!classes.empty()) {
        g_testClass = std::clamp(g_testClass, 0, static_cast<int>(classes.size()) - 1);
        ImGui::SameLine();
        if (ImGui::Button("Test enemy at the laser") && laser) {
            Queue(Pending::Kind::Spawn, laser->ground, classes[static_cast<std::size_t>(g_testClass)]);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("##testclass", classes[static_cast<std::size_t>(g_testClass)].c_str())) {
            for (int i = 0; i < static_cast<int>(classes.size()); ++i) {
                if (ImGui::Selectable(classes[static_cast<std::size_t>(i)].c_str(), i == g_testClass)) g_testClass = i;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    if (nearLaser != nullptr) {
        ImGui::TextDisabled("At the laser: point %d (%s), %.1f m from it", nearLaser->index + 1, nearLaser->added ? "yours" : "the map's",
                            nearLaser->distance / 100.0);
    }
    if (laser && ImGui::CollapsingHeader("Objects at the laser (doors, levers...)")) {
        if (DrawObjects(run, edits, edit, mapKey, base, laser->hit, kLaserObjectReach, "laser")) {
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("Taken out from the next time the map loads.");
    }
    ImGui::Separator();

    // --- the switches and testing points one at a time ----------------------------------------
    {
        bool invulnerable = g_invulnerable.load();
        if (ImGui::Checkbox("Invulnerable", &invulnerable)) {
            g_invulnerable = invulnerable;
            RM_LOGI("map editor: invulnerable %s", invulnerable ? "on" : "off");
        }
        ImGui::SameLine();
        bool hold = g_holdWaves.load();
        if (ImGui::Checkbox("Hold the arena's waves", &hold)) {
            g_holdWaves = hold;
            RM_LOGI("map editor: the arena's waves %s", hold ? "held" : "go on");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Keep testing", &g_keepGoing);
    }
    if (ImGui::CollapsingHeader("Test points one at a time", ImGuiTreeNodeFlags_DefaultOpen)) {
        const std::string cls = classes.empty() ? std::string("uEm10") : classes[static_cast<std::size_t>(g_testClass)];
        const Marked* untested = nullptr;
        for (const Marked& m : points) {
            if (m.distance <= kTooClose || ResultAt(mapKey, m.pos) != nullptr) continue;
            if (untested == nullptr || m.distance < untested->distance) untested = &m;
        }
        ImGui::BeginDisabled(untested == nullptr);
        if (ImGui::Button("Test the nearest untested point") && untested != nullptr) {
            g_testRequest = TestRequest{mapKey, untested->pos, untested->index, cls};
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(nearLaser == nullptr);
        if (ImGui::Button("Test the point at the laser") && nearLaser != nullptr) {
            g_testRequest = TestRequest{mapKey, nearLaser->pos, nearLaser->index, cls};
        }
        ImGui::EndDisabled();
        if (g_test.active) {
            ImGui::SameLine();
            if (ImGui::Button("Stop the test")) {
                g_keepGoing = false;
                g_testRequest.reset();
                g_stopTest = true;  // the tick ends it, with no result
            }
        }
        ImGui::TextDisabled("A %s at the point; it has to come within 3 m of you. Stand still (Invulnerable helps).", cls.c_str());
        const auto results = g_results.find(mapKey);
        int reached = 0, failed = 0;
        if (results != g_results.end()) {
            for (const TestResult& r : results->second) {
                reached += r.kind == TestResult::Kind::Reached ? 1 : 0;
                failed += r.kind == TestResult::Kind::NotReached ? 1 : 0;
            }
        }
        if (!g_testLine.empty()) ImGui::TextWrapped("%s", g_testLine.c_str());
        ImGui::Text("Tested on this map: %d reach you (green ring), %d do not (red ring).", reached, failed);
        ImGui::BeginDisabled(failed == 0);
        const std::string takeAll = "Take out the " + std::to_string(failed) + " that do not";
        if (ImGui::Button(takeAll.c_str()) && failed > 0) {
            int taken = 0;
            for (const TestResult& r : results->second) {
                if (r.kind != TestResult::Kind::NotReached) continue;
                const auto mine = std::find_if(edit.added.begin(), edit.added.end(),
                                               [&](const Point& p) { return DistanceSq(p, r.pos) < 1.0; });
                if (mine != edit.added.end()) {
                    edit.added.erase(mine);
                } else {
                    edit.removed.push_back(r.pos);
                }
                ++taken;
            }
            std::erase_if(results->second, [](const TestResult& r) { return r.kind == TestResult::Kind::NotReached; });
            if (static_cast<std::size_t>(taken) < points.size()) {
                Commit(run, edits, mapKey, std::to_string(taken) + " points that did not reach you taken out");
            }
            ImGui::EndDisabled();
            ImGui::End();
            return;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(reached + failed == 0);
        if (ImGui::Button("Forget the results") && results != g_results.end()) results->second.clear();
        ImGui::EndDisabled();
    }
    ImGui::Separator();

    // --- where the player stands ---------------------------------------------------------------
    ImGui::TextDisabled("You stand at %.0f %.0f %.0f", (*player)[0], (*player)[1], (*player)[2]);
    const Marked* nearest = Nearest(points, kNearReach);
    if (ImGui::Button("Add a spawn point here")) {
        edit.added.push_back(*player);
        Commit(run, edits, mapKey, "spawn point added where you stand");
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(nearest == nullptr || points.size() <= 1);
    if (ImGui::Button("Take out the nearest point") && nearest != nullptr) {
        TakeOut(run, edits, edit, mapKey, *nearest);
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }
    ImGui::EndDisabled();
    // The players' starts (player 2's for a co-op run; the user, 2026-11-21: "set the player spawn
    // point for both players"), where player 1 stands.
    if (ImGui::Button("Set player 1's start here")) {
        edit.start = *player;
        Commit(run, edits, mapKey, "player 1's start set where you stand");
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Set player 2's start here")) {
        edit.start2 = *player;
        Commit(run, edits, mapKey, "player 2's start set where you stand");
        ImGui::End();
        return;
    }
    if (edit.start) {
        if (ImGui::Button("Map's own start")) {
            edit.start.reset();
            Commit(run, edits, mapKey, "the map's own start again");
            ImGui::End();
            return;
        }
    }
    if (edit.start2) {
        if (edit.start) ImGui::SameLine();
        if (ImGui::Button("Player 2 with player 1")) {
            edit.start2.reset();
            Commit(run, edits, mapKey, "player 2 starts at player 1's start again");
            ImGui::End();
            return;
        }
    }
    if (!points.empty()) {
        if (ImGui::Button("Go to the previous point")) {
            g_stepIndex = (g_stepIndex <= 0 ? static_cast<int>(points.size()) : g_stepIndex) - 1;
            Queue(Pending::Kind::Warp, points[static_cast<std::size_t>(g_stepIndex)].pos);
        }
        ImGui::SameLine();
        if (ImGui::Button("Go to the next point")) {
            g_stepIndex = (g_stepIndex + 1) % static_cast<int>(points.size());
            Queue(Pending::Kind::Warp, points[static_cast<std::size_t>(g_stepIndex)].pos);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!laser);
        if (ImGui::Button("Go to the laser") && laser) Queue(Pending::Kind::Warp, laser->ground);
        ImGui::EndDisabled();
        if (g_stepIndex >= 0 && g_stepIndex < static_cast<int>(points.size())) {
            ImGui::SameLine();
            ImGui::TextDisabled("point %d of %zu", g_stepIndex + 1, points.size());
        }
    }
    ImGui::Checkbox("Show the points in the world", &g_showMarkers);
    if (ImGui::CollapsingHeader("Objects near you")) {
        if (DrawObjects(run, edits, edit, mapKey, base, *player, kObjectReach, "near")) {
            ImGui::End();
            return;
        }
        ImGui::TextDisabled("Taken out from the next time the map loads.");
    }
    if (ImGui::CollapsingHeader("Points near you")) {
        std::vector<const Marked*> byDistance;
        for (const Marked& m : points) byDistance.push_back(&m);
        std::sort(byDistance.begin(), byDistance.end(), [](const Marked* a, const Marked* b) { return a->distance < b->distance; });
        ImGui::TextDisabled("blue: the map's, green: yours, red X: taken out, yellow: player 1's start, orange: player 2's,\n"
                            "magenta: the laser");
        for (int i = 0; i < static_cast<int>(byDistance.size()) && i < kListRows; ++i) {
            const Marked& m = *byDistance[static_cast<std::size_t>(i)];
            ImGui::PushID(m.index);
            if (ImGui::SmallButton("Go")) {
                g_stepIndex = m.index;
                Queue(Pending::Kind::Warp, m.pos);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(points.size() <= 1);
            if (ImGui::SmallButton("Take out")) {
                TakeOut(run, edits, edit, mapKey, m);
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::End();
                return;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const TestResult* t = ResultAt(mapKey, m.pos);
            ImGui::Text("%3d  %6.1f m  %-5s %s", m.index + 1, m.distance / 100.0, m.added ? "yours" : "map",
                        t == nullptr ? "" : t->kind == TestResult::Kind::Reached ? "reaches you" : t->kind == TestResult::Kind::NotReached ? "DOES NOT REACH" : "no result");
            ImGui::PopID();
        }
    }

    // --- another map, without leaving the run ---------------------------------------------------
    if (ImGui::CollapsingHeader("Go to another map")) {
        std::vector<const rm::MapDef*> maps;
        for (const rm::MapDef& m : run.content().maps()) maps.push_back(&m);
        std::stable_sort(maps.begin(), maps.end(), [](const rm::MapDef* a, const rm::MapDef* b) { return (a->stage >= 100) < (b->stage >= 100); });
        const rm::MapDef* pick = run.content().FindMapByStage(g_travelPick);
        const bool arena = run.phase() == rm::Phase::ArenaActive;
        ImGui::BeginDisabled(pick == nullptr || !arena);
        const std::string go = pick != nullptr ? "Go to " + pick->name + " now" : std::string("Go there now (choose a map below)");
        if (ImGui::Button(go.c_str()) && pick != nullptr) {
            g_travelRequest = pick->stage;
            g_open = false;
            overlay::SetFocused(false);
            RM_LOGI("map editor: to map %d (%s) asked for", pick->stage, pick->name.c_str());
            ImGui::EndDisabled();
            ImGui::End();
            return;
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(arena ? "The arena starts again on that map (this one's kills do not count)."
                                  : "Only while an arena is being played (not on a reward, shop or route screen).");
        for (const rm::MapDef* m : maps) {
            const auto e = run.map_edits().find(m->stage);
            const bool edited = e != run.map_edits().end() && !e->second.empty();
            const std::string label = m->name + (m->boss ? "  (boss)" : "") + (edited ? "  (edited)" : "") +
                                      (m->stage == mapKey ? "  (here)" : "") + "##" + std::to_string(m->stage);
            if (ImGui::Selectable(label.c_str(), g_travelPick == m->stage)) g_travelPick = m->stage;
        }
    }
    ImGui::Separator();
    if (!g_status.empty()) ImGui::TextWrapped("%s", g_status.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Pad: L3 + R3 open and close, the D-pad moves, A presses, B closes. Keyboard: F8, the mouse. The laser is "
                       "the middle of the screen when the editor opened: aim, then open it. Changes are saved to map_edits.json at "
                       "once and the arena's waves use them at once.");
    ImGui::PopStyleColor();
    ImGui::End();
}

// The point test: start a requested one, follow the one running (game tick).
void EndTest(TestResult::Kind kind, const char* why) {
    const double seconds = Seconds() - g_test.started;
    TestResult r;
    r.pos = g_test.pos;
    r.index = g_test.index;
    r.kind = kind;
    r.seconds = seconds;
    r.closest = g_test.closest;
    Record(g_test.mapKey, r);
    if (g_test.unit && engine::ReadHp(g_test.unit) && engine::ClassName(g_test.unit) == g_test.unitClass) director::Kill(g_test.unit);
    char line[160];
    std::snprintf(line, sizeof(line), "Point %d: %s (%.0f s; it came within %.1f m).", g_test.index + 1, why, seconds,
                  g_test.closest / 100.0);
    g_testLine = line;
    g_testLineUntil = Seconds() + 6.0;
    RM_LOGI("map editor: test of point %d (%.0f %.0f %.0f) on map %d: %s after %.1f s, closest %.1f m", g_test.index + 1, g_test.pos[0],
            g_test.pos[1], g_test.pos[2], g_test.mapKey, why, seconds, g_test.closest / 100.0);
    g_test.active = false;
    g_test.unit = 0;
    g_nextAutoTest = g_keepGoing ? Seconds() + kKeepGoingPause : 0.0;
}

void TickTest(const Frame& frame) {
    const std::optional<Point> player = PlayerPosition();
    if (g_test.active && (!frame.inMatch || !player || frame.mapKey != g_test.mapKey)) {
        g_test.active = false;  // the match or the map went away: no result
        g_test.unit = 0;
        g_testLine.clear();
    }
    // Keep going: the nearest untested point once the last result has shown a moment.
    if (!g_test.active && !g_testRequest && g_keepGoing && g_nextAutoTest > 0.0 && Seconds() >= g_nextAutoTest && player &&
        frame.run != nullptr) {
        g_nextAutoTest = 0.0;
        const std::vector<Marked> points = Points(*frame.run, frame.mapKey, player);
        const Marked* untested = nullptr;
        for (const Marked& m : points) {
            if (m.distance <= kTooClose || ResultAt(frame.mapKey, m.pos) != nullptr) continue;
            if (untested == nullptr || m.distance < untested->distance) untested = &m;
        }
        if (untested != nullptr) {
            const std::vector<std::string> classes = TestClasses(*frame.run, frame.mapKey);
            const std::string cls = classes.empty() ? std::string("uEm10")
                                                    : classes[static_cast<std::size_t>(std::clamp(g_testClass, 0, static_cast<int>(classes.size()) - 1))];
            g_testRequest = TestRequest{frame.mapKey, untested->pos, untested->index, cls};
        } else {
            g_keepGoing = false;
            g_testLine = "Every point of this map has a result.";
            g_testLineUntil = Seconds() + 6.0;
        }
    }
    if (g_testRequest && frame.inMatch && player) {
        const TestRequest request = *g_testRequest;
        g_testRequest.reset();
        if (g_test.active) EndTest(TestResult::Kind::Lost, "test stopped for the next one");
        director::SpawnSpec spec;
        spec.unitClass = request.unitClass;
        for (int k = 0; k < 3; ++k) spec.pos[k] = static_cast<float>(request.pos[static_cast<std::size_t>(k)]);
        g_test = {};
        g_test.mapKey = request.mapKey;
        g_test.pos = request.pos;
        g_test.index = request.index;
        g_test.unitClass = request.unitClass;
        g_test.started = Seconds();
        g_test.distance = std::sqrt(DistanceSq(request.pos, *player));
        g_test.closest = g_test.distance;
        g_test.limit = std::min(kTestMax, kTestBase + g_test.distance / 100.0 * kTestPerMetre);
        g_test.unit = director::Spawn(spec);
        g_test.active = true;
        if (!g_test.unit) {
            EndTest(TestResult::Kind::NoSpawn, "the test enemy could not be spawned");
            return;
        }
        RM_LOGI("map editor: testing point %d on map %d with %s, %.1f m away, %.0f s", request.index + 1, request.mapKey,
                request.unitClass.c_str(), g_test.distance / 100.0, g_test.limit);
    }
    if (!g_test.active || !player) return;
    if (std::exchange(g_stopTest, false)) {
        if (g_test.unit && engine::ReadHp(g_test.unit) && engine::ClassName(g_test.unit) == g_test.unitClass) director::Kill(g_test.unit);
        g_test.active = false;
        g_test.unit = 0;
        g_nextAutoTest = 0.0;
        g_testLine = "Test stopped.";
        g_testLineUntil = Seconds() + 3.0;
        RM_LOGI("map editor: test of point %d stopped", g_test.index + 1);
        return;
    }
    const auto hp = engine::ReadHp(g_test.unit);
    if (!hp || hp->hp <= 0 || engine::ClassName(g_test.unit) != g_test.unitClass) {
        EndTest(TestResult::Kind::Lost, "the test enemy died or went away first");
        return;
    }
    const Point at{ReadOr<float>(g_test.unit + re5::off::kUnitPosition, 0.0f), ReadOr<float>(g_test.unit + re5::off::kUnitPosition + 4, 0.0f),
                   ReadOr<float>(g_test.unit + re5::off::kUnitPosition + 8, 0.0f)};
    const double distance = std::sqrt(DistanceSq(at, *player));
    g_test.closest = std::min(g_test.closest, distance);
    const double elapsed = Seconds() - g_test.started;
    if (distance <= kReached) {
        EndTest(TestResult::Kind::Reached, "it REACHES you");
        return;
    }
    if (elapsed >= g_test.limit) {
        EndTest(TestResult::Kind::NotReached, "it does NOT reach you");
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "Testing point %d: the %s is %.1f m from you, %.0f of %.0f s.", g_test.index + 1,
                  g_test.unitClass.c_str(), distance / 100.0, elapsed, g_test.limit);
    g_testLine = line;
    g_testLineUntil = Seconds() + 1.0;
}

}  // namespace

void Load(const std::filesystem::path& dataDir, rm::RunController& run) {
    g_file = dataDir / L"map_edits.json";
    std::error_code ec;
    if (!std::filesystem::exists(g_file, ec)) return;
    std::ifstream in(g_file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string error;
    const auto edits = rm::ParseMapEdits(text.str(), &error);
    if (!edits) {
        RM_LOGW("map editor: map_edits.json not used: %s", error.c_str());
        return;
    }
    run.SetMapEdits(*edits);
    for (const auto& [key, edit] : *edits) {
        RM_LOGI("map editor: map %d: %zu points added, %zu taken out, %zu objects hidden%s (map_edits.json)", key, edit.added.size(),
                edit.removed.size(), edit.hidden.size(), edit.start ? ", its own start" : "");
    }
}

void SetEnabled(bool on) {
    g_enabled = on;
    overlay::SetEditorKey(on ? VK_F8 : 0);
    if (on) RM_LOGI("map editor: on (F8 or L3 + R3 in an arena)");
}

bool Enabled() { return g_enabled.load(); }
int Serial() { return g_serial.load(); }
bool Open() { return g_open && overlay::IsFocused(); }
unsigned short PadCombo() { return g_enabled.load() ? kPadCombo : 0; }

int TakeTravelRequest() { return std::exchange(g_travelRequest, -1); }

void EditorRunStarting() { g_editorRunPending = true; }
bool Invulnerable() { return g_enabled.load() && g_invulnerable.load(); }
bool HoldWaves() { return g_enabled.load() && g_holdWaves.load(); }

void Draw(const Frame& frame) {
    if (!g_enabled.load() || frame.run == nullptr) return;
    if (overlay::TakeEditorPresses() > 0) {
        g_open = !(g_open && overlay::IsFocused());
        overlay::SetFocused(g_open);
        if (g_open) {
            g_used = true;
            g_justOpened = true;
            g_laserAtOpen = g_laser;
            RM_LOGI("map editor: opened on map %d (laser %s)", frame.mapKey, g_laser ? "on the world" : "on nothing");
        }
    }
    if (g_open && !overlay::IsFocused()) g_open = false;  // F7 took the focus away
    if (!frame.inMatch) return;
    const std::optional<Point> player = PlayerPosition();
    if (g_used && g_showMarkers) DrawMarkers(*frame.run, frame.mapKey, player);
    if (g_open) DrawWindow(*frame.run, frame.mapKey, player);
    // The point test's line at the top of the screen, while it runs and a moment after.
    if (!g_open && !g_testLine.empty() && Seconds() < g_testLineUntil) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 size = ImGui::CalcTextSize(g_testLine.c_str());
        const ImVec2 at((display.x - size.x) * 0.5f, display.y * 0.12f);
        ImDrawList* list = ImGui::GetForegroundDrawList();
        list->AddRectFilled(ImVec2(at.x - 10.0f, at.y - 6.0f), ImVec2(at.x + size.x + 10.0f, at.y + size.y + 6.0f), IM_COL32(0, 0, 0, 170), 6.0f);
        list->AddText(at, IM_COL32(255, 235, 140, 255), g_testLine.c_str());
    }
}

void Tick(const Frame& frame) {
    if (!g_enabled.load()) return;
    // L3 + R3 on player 1's pad (owned while in a match: the game does not see them together).
    const unsigned short pad = RawXInputButtons();
    if ((pad & kPadCombo) == kPadCombo && (g_prevPad & kPadCombo) != kPadCombo && frame.inMatch) overlay::PressEditorKey();
    g_prevPad = pad;
    if (frame.inMatch && !g_open) UpdateLaser();
    // The switches belong to a run: on in one MAP EDITOR started, off once it is over.
    const bool running = frame.run != nullptr && frame.run->HasActiveRun();
    if (running && g_editorRunPending) {
        g_editorRunPending = false;
        g_invulnerable = true;
        RM_LOGI("map editor: a MAP EDITOR run: invulnerable");
    } else if (!running && !g_editorRunPending && (g_invulnerable.load() || g_holdWaves.load())) {
        g_invulnerable = false;
        g_holdWaves = false;
        g_keepGoing = false;
        RM_LOGI("map editor: the run is over: invulnerable and held waves off");
    }
    TickTest(frame);
    if (g_pending.empty()) return;
    std::vector<Pending> work;
    work.swap(g_pending);
    if (!frame.inMatch) return;
    for (const Pending& p : work) {
        if (p.kind == Pending::Kind::Warp) {
            const int copies = engine::WarpUnit(engine::LocalPlayer(), p.pos);
            RM_LOGI("map editor: player sent to %.0f %.0f %.0f (%d copies)", p.pos[0], p.pos[1], p.pos[2], copies);
            g_status = copies > 0 ? "Sent there." : "Could not move the player.";
        } else if (p.kind == Pending::Kind::Spawn) {
            director::SpawnSpec spec;
            spec.unitClass = p.unitClass;
            std::copy(std::begin(p.pos), std::end(p.pos), spec.pos);
            const std::uintptr_t unit = director::Spawn(spec);
            RM_LOGI("map editor: test %s at %.0f %.0f %.0f -> %08X", p.unitClass.c_str(), p.pos[0], p.pos[1], p.pos[2],
                    static_cast<unsigned>(unit));
            g_status = unit ? "Test " + p.unitClass + " spawned: does it reach you?" : "The test enemy could not be spawned.";
        }
    }
}

}  // namespace rmre5::mapeditor
