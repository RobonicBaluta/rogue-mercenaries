#pragma once

// Dear ImGui overlay drawn on the game's D3D9 device from the Present hook
// (render thread). Input arrives through a subclassed WndProc.

struct IDirect3DDevice9;

namespace rmre5::overlay {

using DrawCallback = void (*)();

void SetDrawCallback(DrawCallback callback);
void SetToggleKey(int virtualKey);
// A second key (the map editor's F8; 0: none): its presses are counted for the draw callback,
// which decides the focus itself. The game does not see the key.
void SetEditorKey(int virtualKey);
unsigned TakeEditorPresses();
// The editor's pad combo was pressed (counted like its key).
void PressEditorKey();

// While focused the overlay shows a cursor, takes keyboard/mouse and the game
// receives neutral input (see HookSink::BlockGameInput).
bool IsFocused();
void SetFocused(bool focused);

bool IsInitialized();

// The game window (an HWND) the overlay draws over; null until the first frame.
void* WindowHandle();

// The device being drawn on, inside the draw callback only (else nullptr).
IDirect3DDevice9* FrameDevice();

// Menu hotkeys pressed while focused since the last call: bit n (1..9) for the
// digit keys, kHotkeyEnter for Enter. Read by the draw callback.
inline constexpr unsigned kHotkeyEnter = 1u << 10;
unsigned TakeHotkeys();

// Called by the input hooks with the game's own DirectInput state before it
// is blanked. Works even when the game holds the devices exclusively.
void FeedKeyboard(const unsigned char* keys256);
void FeedMouse(long dx, long dy, long wheel, const unsigned char* buttons4);

// Called by the D3D9 hooks.
void OnPresent(IDirect3DDevice9* device);
void OnBeforeReset();
void OnAfterReset();

}  // namespace rmre5::overlay
