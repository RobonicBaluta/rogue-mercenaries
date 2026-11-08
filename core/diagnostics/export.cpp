#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <system_error>

#include "rm/diagnostics.h"
#include "rm/version.h"
#include "rm/zip.h"
#include "rm/common.h"

namespace rm {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool HasExtension(const fs::path& p, std::initializer_list<std::string_view> allowed) {
    const std::string ext = Lower(rm::PathText(p.extension()));
    return std::find(allowed.begin(), allowed.end(), ext) != allowed.end();
}

bool ReadAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::vector<fs::path> ListFiles(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

DiagnosticsReport ExportDiagnostics(const fs::path& out_zip, const fs::path& log_dir, const fs::path& config_dir,
                                    const std::vector<fs::path>& extra_files, const std::string& summary_text,
                                    const DiagnosticsOptions& options) {
    DiagnosticsReport report;
    std::vector<ZipEntry> entries;
    std::uint64_t total = 0;
    std::vector<std::string> error_lines;

    auto add = [&](const fs::path& p, const std::string& archive_name, std::string* content_out) {
        std::error_code ec;
        const fs::file_status st = fs::symlink_status(p, ec);
        if (ec || fs::is_symlink(st) || !fs::is_regular_file(st)) {
            report.skipped.push_back(rm::PathText(p) + ": not a regular file");
            return false;
        }
        const std::uint64_t size = fs::file_size(p, ec);
        if (ec || size > options.max_file_bytes) {
            report.skipped.push_back(rm::PathText(p) + ": too large");
            return false;
        }
        if (total + size > options.max_total_bytes) {
            report.skipped.push_back(rm::PathText(p) + ": archive size limit reached");
            return false;
        }
        for (const ZipEntry& e : entries) {
            if (e.name == archive_name) {
                report.skipped.push_back(rm::PathText(p) + ": duplicate name " + archive_name);
                return false;
            }
        }
        std::string data;
        if (!ReadAll(p, data)) {
            report.skipped.push_back(rm::PathText(p) + ": unreadable");
            return false;
        }
        total += data.size();
        if (content_out != nullptr) *content_out = data;
        entries.push_back(ZipEntry{archive_name, std::move(data)});
        report.included.push_back(archive_name);
        return true;
    };

    for (const fs::path& p : ListFiles(log_dir)) {
        if (!HasExtension(p, {".jsonl", ".log", ".txt"})) continue;
        std::string content;
        if (!add(p, "logs/" + rm::PathText(p.filename()), &content)) continue;
        if (Lower(rm::PathText(p.extension())) != ".jsonl") continue;
        std::istringstream lines(content);
        for (std::string line; std::getline(lines, line);) {
            if (line.find("\"level\":\"error\"") != std::string::npos) error_lines.push_back(line);
        }
    }
    for (const fs::path& p : ListFiles(config_dir)) {
        if (HasExtension(p, {".json"})) add(p, "configs/" + rm::PathText(p.filename()), nullptr);
    }
    for (const fs::path& p : extra_files) {
        if (!HasExtension(p, {".jsonl", ".json", ".log", ".txt", ".ini", ".md", ".csv"})) {
            report.skipped.push_back(rm::PathText(p) + ": file type not allowed");
            continue;
        }
        add(p, "extra/" + rm::PathText(p.filename()), nullptr);
    }

    // JSONL lines start with {"ts":"<ISO-8601>", so ordering by that prefix is chronological;
    // the stable sort keeps file order for equal timestamps.
    auto ts_key = [](const std::string& line) {
        return std::string_view(line).substr(0, std::min<std::size_t>(line.size(), 31));
    };
    std::stable_sort(error_lines.begin(), error_lines.end(),
                     [&ts_key](const std::string& a, const std::string& b) { return ts_key(a) < ts_key(b); });
    const std::size_t first =
        error_lines.size() > options.max_errors ? error_lines.size() - options.max_errors : 0;

    std::ostringstream summary;
    summary << "Rogue Mercenaries diagnostic export\n"
            << "mod_version: " << kModVersion << "\n\n"
            << summary_text << "\n\n"
            << "last " << (error_lines.size() - first) << " of " << error_lines.size() << " error events:\n";
    for (std::size_t i = first; i < error_lines.size(); ++i) summary << "  " << error_lines[i] << "\n";
    summary << "\nincluded files:\n";
    for (const std::string& name : report.included) summary << "  " << name << "\n";
    if (!report.skipped.empty()) {
        summary << "\nskipped:\n";
        for (const std::string& s : report.skipped) summary << "  " << s << "\n";
    }
    entries.insert(entries.begin(), ZipEntry{"summary.txt", summary.str()});
    report.included.insert(report.included.begin(), "summary.txt");

    report.status = WriteStoredZip(out_zip, entries);
    return report;
}

}  // namespace rm
