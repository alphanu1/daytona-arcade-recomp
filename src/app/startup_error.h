#pragma once
#include <SDL3/SDL.h>
#include <string>

namespace app {
inline int startup_error(const char *operation) {
    // Capture before logging/showing a dialog, which can change SDL's error.
    const std::string message = std::string(operation) + " failed: " + SDL_GetError();
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", message.c_str());
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Daytona Error", message.c_str(), nullptr))
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not show error dialog: %s", SDL_GetError());
    return 1;
}
} // namespace app
