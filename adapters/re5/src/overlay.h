#pragma once

// Dear ImGui overlay drawn on the game's D3D9 device from the Present hook
// (render thread). Input arrives through a subclassed WndProc.

struct IDirect3DDevice9;

namespace rmre5::overlay {

using DrawCallback = void (*)();

void SetDrawCallback(DrawCallback callback);
void SetToggleKey(int virtualKey);

// While focused the overlay shows a cursor, takes keyboard/mouse and the game
// receives neutral input (see HookSink::BlockGameInput).
bool IsFocused();
void SetFocused(bool focused);

bool IsInitialized();

// Called by the input hooks with the game's own DirectInput state before it
// is blanked. Works even when the game holds the devices exclusively.
void FeedKeyboard(const unsigned char* keys256);
void FeedMouse(long dx, long dy, long wheel, const unsigned char* buttons4);

// Called by the D3D9 hooks.
void OnPresent(IDirect3DDevice9* device);
void OnBeforeReset();
void OnAfterReset();

}  // namespace rmre5::overlay
