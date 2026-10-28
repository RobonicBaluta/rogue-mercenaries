#pragma once

// Native "ROGUE MERCENARIES" row and mod menu inside RE5's Play menu; see
// native_menu.cpp. The screens' content comes from a Model that mod.cpp
// implements on top of the run controller.

#include <string>
#include <vector>

namespace rmre5::nativemenu {

enum class ScreenId { Main = 0, Characters, Perks, Credits, Skins, PerkList };

struct Item {
    std::string label;  // one row; rendered in capitals by the game font
    std::string help;   // up to two lines under the list
    bool locked = false;  // drawn in the game's "disabled" red
};

struct Screen {
    std::string header;  // small line above the list
    std::vector<Item> items;
    std::string empty_help;  // help line when there are no items
};

enum class Outcome {
    Stay,      // nothing happened
    Refresh,   // the screen content changed (rebuild it)
    Push,      // open `next`
    Back,      // close this screen
    StartRun,  // leave the mod menu into the game's Mercenaries flow
    Denied,    // play the game's error sound
};

struct Decision {
    Outcome outcome = Outcome::Stay;
    ScreenId next = ScreenId::Main;
};

// Implemented by mod.cpp. Both calls arrive on the game's menu thread.
struct Model {
    virtual ~Model() = default;
    virtual Screen Build(ScreenId id) = 0;
    virtual Decision Decide(ScreenId id, int index) = 0;
};
void SetModel(Model* model);

// Queues the Play menu hooks; call before MH_ApplyQueued.
bool Install();
// Game thread: restores the borrowed messages when the menu has closed.
void OnGameTick();
// True while the Play menu is on screen and waiting for input (not while it
// fades out after a choice).
bool MenuAtInput();
// True once (then cleared) after the player chose START RUN.
bool TakeChosen();
bool Chosen();

}  // namespace rmre5::nativemenu
