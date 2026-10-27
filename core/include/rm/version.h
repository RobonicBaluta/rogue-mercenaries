#pragma once

namespace rm {

/// Version of the portable core; written into every telemetry line and diagnostic summary.
inline constexpr const char* kModVersion = "0.1.0-dev";

/// Current persistent profile schema. Older files are migrated on load (see profile.h).
inline constexpr int kProfileSchemaVersion = 1;

/// Content (configs/*.json) schema understood by this build.
inline constexpr int kContentSchemaVersion = 1;

}  // namespace rm
