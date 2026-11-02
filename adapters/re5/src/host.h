#pragma once

// Arena host (ROADMAP.md, milestone M5): which stage a match loads. The Mercenaries setup
// (FUN_00BF5050, run when the Solo flow starts the match) maps the stage choice 0..7 through
// the table at 0x010CB9B0 to the stage numbers 400..407 (the Mercenaries stages s400..s407)
// and stores the number in the save's progress (`[0x011B2158] + slot*0x5940 + 0x273D8`).
// The match's scene switch (FUN_0071D8B0 -> FUN_00716C80) then creates the scene class
// aS<stage> and the area load reads Image\Archive\s<stage>. Campaign stages are s1xx..s6xx.
namespace rmre5::host {

/// Queues the hook on the Mercenaries match setup.
bool Install();
bool Available();

/// The next Mercenaries matches load stage `stage` instead of the chosen Mercenaries one
/// (-1: no override).
void SetStageOverride(int stage);
int StageOverride();

/// The stage number the save holds for the current/next match (-1 unknown).
int SavedStage();

}  // namespace rmre5::host
