// rm_fxgen <game>\nativePC_MT\Image\Archive <out.arc>: builds the mod's effects archive from the
// game's uWp21.arc and coreEff.arc exactly as the adapter does at start (rm::fx::BuildFlameEffects),
// to check its output by hand (scripts/arc_extract.py). Reads the game's files, writes only <out.arc>.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "rm/effects.h"

namespace {

bool ReadFile(const std::filesystem::path& path, rm::fx::Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: rm_fxgen <game>\\nativePC_MT\\Image\\Archive <out.arc>\n");
        return 2;
    }
    const std::filesystem::path dir(argv[1]);
    rm::fx::Bytes wp21, core;
    if (!ReadFile(dir / "uWp21.arc", wp21) || !ReadFile(dir / "coreEff.arc", core)) {
        std::fprintf(stderr, "uWp21.arc or coreEff.arc not read from %s\n", argv[1]);
        return 1;
    }
    std::vector<std::string> log;
    const auto archive = rm::fx::BuildFlameEffects(wp21, core, &log);
    for (const std::string& line : log) std::printf("%s\n", line.c_str());
    if (!archive) return 1;
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(archive->data()), static_cast<std::streamsize>(archive->size()));
    std::printf("written %s\n", argv[2]);
    return out ? 0 : 1;
}
