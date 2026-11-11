#include "effects_pack.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include "log.h"
#include "rm/effects.h"

namespace rmre5::effectspack {
namespace {

std::mutex g_mutex;
std::filesystem::path g_ready;

bool ReadFile(const std::filesystem::path& path, rm::fx::Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
}

// What the archive was built from: the builder's version and its sources' sizes and times.
std::string Stamp(const std::filesystem::path& wp21, const std::filesystem::path& core) {
    std::error_code ec1, ec2, ec3, ec4;
    const auto s1 = std::filesystem::file_size(wp21, ec1), s2 = std::filesystem::file_size(core, ec2);
    const auto t1 = std::filesystem::last_write_time(wp21, ec3), t2 = std::filesystem::last_write_time(core, ec4);
    if (ec1 || ec2 || ec3 || ec4) return {};
    return "v" + std::to_string(rm::fx::kEffectsVersion) + " " + std::to_string(s1) + " " + std::to_string(t1.time_since_epoch().count()) +
           " " + std::to_string(s2) + " " + std::to_string(t2.time_since_epoch().count());
}

void Build(Paths paths) {
    const auto archives = paths.gameDir / L"nativePC_MT" / L"Image" / L"Archive";
    const auto wp21Path = archives / L"uWp21.arc", corePath = archives / L"coreEff.arc";
    const auto dir = paths.dataDir / L"generated";
    const auto out = dir / (std::string(rm::fx::kEffectsArchive) + ".arc");
    const auto stampPath = dir / (std::string(rm::fx::kEffectsArchive) + ".stamp");
    const std::string stamp = Stamp(wp21Path, corePath);
    if (stamp.empty()) {
        RM_LOGW("effects: uWp21.arc or coreEff.arc not found in %s; the mod's own effects are off", archives.string().c_str());
        return;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(out, ec) && ReadText(stampPath) == stamp) {
        std::lock_guard lock(g_mutex);
        g_ready = out;
        RM_LOGI("effects: %s kept (built before from the same game files)", out.string().c_str());
        return;
    }
    rm::fx::Bytes wp21, core;
    if (!ReadFile(wp21Path, wp21) || !ReadFile(corePath, core)) {
        RM_LOGW("effects: the game's uWp21.arc or coreEff.arc could not be read; the mod's own effects are off");
        return;
    }
    const DWORD started = GetTickCount();
    std::vector<std::string> log;
    const auto archive = rm::fx::BuildFlameEffects(wp21, core, &log);
    if (!archive) {
        RM_LOGW("effects: not built (%s); the mod's own effects are off", log.empty() ? "?" : log.back().c_str());
        return;
    }
    std::filesystem::create_directories(dir, ec);
    const auto temp = dir / (std::string(rm::fx::kEffectsArchive) + ".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(archive->data()), static_cast<std::streamsize>(archive->size()));
        if (!file) {
            RM_LOGW("effects: %s could not be written; the mod's own effects are off", temp.string().c_str());
            return;
        }
    }
    std::filesystem::rename(temp, out, ec);
    if (ec) {
        RM_LOGW("effects: %s could not be put in place (%s); the mod's own effects are off", out.string().c_str(), ec.message().c_str());
        return;
    }
    {
        std::ofstream file(stampPath, std::ios::binary | std::ios::trunc);
        file << stamp;
    }
    for (const std::string& line : log) RM_LOGI("effects: %s", line.c_str());
    std::lock_guard lock(g_mutex);
    g_ready = out;
    RM_LOGI("effects: %s built in %lu ms", out.string().c_str(), static_cast<unsigned long>(GetTickCount() - started));
}

}  // namespace

void Start(const Paths& paths) {
    std::thread([paths] {
        try {
            Build(paths);
        } catch (const std::exception& e) {
            RM_LOGW("effects: building faulted (%s); the mod's own effects are off", e.what());
        }
    }).detach();
}

std::filesystem::path Ready() {
    std::lock_guard lock(g_mutex);
    return g_ready;
}

}  // namespace rmre5::effectspack
