#include "windowlevel.h"

#include <SDL.h>
#include <SDL_syswm.h>

#import <Cocoa/Cocoa.h>

namespace tessera {

void setAboveMenuBar(SDL_Window* window, bool above)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (window == nullptr || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_COCOA) {
        return;
    }

    @autoreleasepool {
        // The level SDL gives the focused fullscreen window
        NSWindow* nsWindow = info.info.cocoa.window;
        const NSWindowLevel level = above ? NSMainMenuWindowLevel + 1 : NSNormalWindowLevel;
        if (nsWindow.level != level) {
            nsWindow.level = level;
        }
    }
}

}
