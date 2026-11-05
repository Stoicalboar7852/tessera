// Tessera multi-display: macOS window levels for the stream windows (docs/phase-3-design.md, component 5).
#pragma once

struct SDL_Window;

namespace tessera {

// macOS: puts a fullscreen stream window above the menu bar, or back at the normal level. SDL keeps only the window
// with keyboard focus above the menu bar, so with several fullscreen windows the menu bar would cover the others.
// Implemented in windowlevel_mac.mm.
void setAboveMenuBar(SDL_Window* window, bool above);

}
