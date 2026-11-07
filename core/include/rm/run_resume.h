#pragma once

#include <string>
#include <string_view>

#include "rm/run_state.h"
#include "rm/status.h"

namespace rm {

/// Version of the resume point's JSON (rm::SerializeRunResume); another version is refused.
inline constexpr int kRunResumeVersion = 1;

/// A run between arenas as JSON: what it needs to go on at its next arena (ids, run map, ledger,
/// weapons, perks, stats, keys, results so far). Not written: the character (rebuilt from content
/// by the controller), offers, combat bookkeeping and cooldown times (session clocks).
std::string SerializeRunResume(const RunState& run);

/// Reads SerializeRunResume's JSON into `out` (a fresh RunState; the character left empty).
Status ParseRunResume(std::string_view text, RunState& out);

}  // namespace rm
