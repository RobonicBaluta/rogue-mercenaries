#pragma once

// Drives the game's own Mercenaries setup screens after START RUN; see
// autopilot.cpp.

#include <cstdint>
#include <utility>
#include <vector>

namespace rmre5::autopilot {

struct Plan {
    int stage = 0;        // uIdStartUp21 row (0..stage_count-1)
    int costume = 0;      // uIdStartUp22 slot (the game's character/costume index)
    int stage_count = 8;  // rows to cycle through when the game refuses a locked stage
    // M10 local co-op: MODE SELECT's row (0 Solo, 2 Split screen) and player 2's costume slot (-1:
    // the game's default). With 2 the stage is answered as in Solo, both costumes are written, and
    // the two-player lobby is the game's (player 2 joins with START), unless skip_lobby: a later
    // arena of a co-op run, whose lobby fields (SetLobby) are written back and the lobby skipped.
    int mode = 0;
    int costume2 = -1;
    bool skip_lobby = false;
};
/// M10: the split-screen lobby's game work fields once both players are in (read at a co-op run's
/// first match), written back when a later arena's lobby is skipped. Empty: none known.
void SetLobby(const std::vector<std::pair<int, std::uint32_t>>& fields);
bool LobbyKnown();

// Queues the hooks; call before MH_ApplyQueued.
bool Install();
bool Available();
// Starts driving the next screens (uIdStartUp20 -> 21 -> 22) with this plan.
/// True when a match can be restarted on another stage without any screen: the flow
/// script hooks and the results screen hook are in place.
bool CanChangeStage();
void Arm(const Plan& plan);
void Disarm(const char* reason);
bool Armed();
/// After a run's last match: its Mercenaries results screen is skipped like a stage change's
/// (no stats page, no record written to the game's save) and the Mercenaries menu script's
/// first step answers "back", so the game returns to its title Play menu. Cleared by Arm and
/// once the script has taken it. Needs CanChangeStage().
void ReturnToTitle(bool on);
bool ReturningToTitle();
/// A run's match is on (the mod sets it every tick): if it is quit (the pause menu, the game
/// over), the match's quit state finds the Mercenaries phase cleared and writes no save.
void SetRunMatch(bool on);
/// While on, the game's save file write finishes without writing (re5::addr::kFnSaveFileWrite):
/// the mod sets it while a run is on and for a while after its match, so no way of leaving a
/// run's match (the pause menu's quit among them) changes savedata.bin.
void SetSaveGuard(bool on);
// Game thread: times out an autopilot that could not finish.
void OnGameTick();

/// Developer mode (M10 F0): the next time the game's Mercenaries menu script asks Iniciar/Solo/Duo
/// while the autopilot is not armed, it is answered with Iniciar and this mode (game work +0x5CC: 0 Solo,
/// 1 Duo online, 2 Split screen) and the screens after it are the game's own. -1 clears it.
void SetDevMercenariesMode(int mode);
/// Menu input bits added to the game's menu input query (kFnMenuInput) on this thread until
/// cleared with 0, and the pad's decide bits: a caller feeds a screen's move() a decide press.
void InjectMenuBits(unsigned bits);
unsigned DecideBits();

}  // namespace rmre5::autopilot
