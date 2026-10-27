#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rm/status.h"

namespace rm {

struct DiagnosticsOptions {
    std::size_t max_errors = 20;                        // last N error lines in summary.txt
    std::uint64_t max_file_bytes = 8u * 1024u * 1024u;  // larger files are skipped
    std::uint64_t max_total_bytes = 48u * 1024u * 1024u;
};

struct DiagnosticsReport {
    Status status;
    std::vector<std::string> included;  // archive paths
    std::vector<std::string> skipped;   // "path: reason"
};

/// Packs a support ZIP:
///   logs/<file>     *.jsonl, *.log, *.txt directly inside log_dir (not recursive)
///   configs/<file>  *.json directly inside config_dir (not recursive)
///   extra/<file>    each extra file with a text extension (.jsonl .json .log .txt .ini .md .csv)
///   summary.txt     mod version, summary_text (adapter build identity etc.), last N errors
/// Never includes executables, DLLs, game archives, saves or anything outside these inputs;
/// symlinks and oversized files are skipped and listed in the report.
DiagnosticsReport ExportDiagnostics(const std::filesystem::path& out_zip,
                                    const std::filesystem::path& log_dir,
                                    const std::filesystem::path& config_dir,
                                    const std::vector<std::filesystem::path>& extra_files,
                                    const std::string& summary_text,
                                    const DiagnosticsOptions& options = {});

}  // namespace rm
