#pragma once

// The mouse on the mod's native screens: the Play menu's mod rows (and its ROGUE MERCENARIES row)
// and the in-match screens (rewards, shop, route...). Hovering a row moves the cursor to it, a
// click chooses it, the right button goes back and the wheel scrolls. See menu_mouse.cpp.

namespace rmre5::menumouse {

enum class Host { PlayMenu = 0, Match = 1 };

/// The mouse since the host's previous frame. Positions are on the game's 1280x720 menu canvas
/// (the one its menu text routine lays rows out on).
struct Input {
    bool valid = false;          // the pointer is over the game's picture
    bool windowsCursor = false;  // the pointer is the Windows cursor (else the mod moves its own)
    float x = 0.0f;
    float y = 0.0f;
    bool moved = false;          // the pointer moved since the host's last frame
    int leftPresses = 0;         // left button presses since then
    int rightPresses = 0;
    bool leftHeld = false;       // the button is down now
    bool rightHeld = false;
    int wheelSteps = 0;          // wheel notches since then; + is away from the player (scroll up)

    [[nodiscard]] bool Over(float centreX, float centreY, float halfWidth, float halfHeight) const {
        return valid && x >= centreX - halfWidth && x <= centreX + halfWidth && y >= centreY - halfHeight &&
               y < centreY + halfHeight;
    }
};

/// Queues a watch on SetCursorPos (whether the game is moving the Windows cursor itself, as it does
/// to keep it centred during play). Call before MH_ApplyQueued.
bool Install();
/// The input hooks: every DirectInput read of the mouse (relative counts, wheel, the first four
/// buttons), before the game's copy is filtered.
void Feed(long dx, long dy, long wheel, const unsigned char* buttons4);
/// One frame of a host: its mouse input since its previous frame. `modScreen`: one of the mod's own
/// screens is up (the pointer may be drawn; an in-match screen also keeps the mouse from the game).
Input Take(Host host, bool modScreen);
/// The keyboard or a pad moved the host's cursor: the drawn pointer hides until the mouse moves.
void KeyNavigated();
/// True while an in-match mod screen is up: the game then sees no mouse (no camera turn, no shot
/// under the screen; a button held when it closes stays hidden until released).
bool CaptureGameMouse();
/// Render thread, inside the overlay's draw: this frame's pointer position, and the mod's own
/// pointer when Windows shows none.
void OnDraw();

}  // namespace rmre5::menumouse
