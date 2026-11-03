#pragma once

// The game's Organize/Buy screen (uIdItemBox00) with a run in it (ROADMAP M7.1): Buy and Upgrade
// belong to the mod's SHOP, so while the guard is on the screen keeps its top-menu cursor off
// "Buy" and turns an "Upgrade" back into the organize grid. Organize, combine, details and sell
// stay the game's. A hook on the screen's per-player panel update.
namespace rmre5::itemboxguard {

bool Install();
/// On while the run is in the screen (the mod's own item box visit); off for the campaign's.
void SetGuard(bool on);

}  // namespace rmre5::itemboxguard
