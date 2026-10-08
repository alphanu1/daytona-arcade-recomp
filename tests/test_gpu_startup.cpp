#include "app/gpu_device.h"
#include "app/startup_error.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace {
std::map<std::string, bool> properties;
bool fail_allocate = false, fail_property = false, fail_device = false, fail_dialog = false;
int destroyed = 0, created = 0, dialogs = 0, logs = 0;
std::string error = "No supported SDL_GPU backend found!", shown;
void check(bool ok) { if (!ok) std::abort(); }
}
extern "C" {
SDL_PropertiesID SDLCALL SDL_CreateProperties() { return fail_allocate ? 0 : 1; }
bool SDLCALL SDL_SetBooleanProperty(SDL_PropertiesID id, const char *name, bool value) {
    check(id == 1); if (fail_property) return false; properties[name] = value; return true;
}
void SDLCALL SDL_DestroyProperties(SDL_PropertiesID id) { check(id == 1); ++destroyed; }
SDL_GPUDevice *SDLCALL SDL_CreateGPUDeviceWithProperties(SDL_PropertiesID id) {
    check(id == 1); ++created;
    return fail_device ? nullptr : reinterpret_cast<SDL_GPUDevice *>(1);
}
const char *SDLCALL SDL_GetError() { return error.c_str(); }
void SDLCALL SDL_LogError(int, const char *, ...) { ++logs; error = "logging changed error"; }
bool SDLCALL SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *parent) {
    check(flags == SDL_MESSAGEBOX_ERROR && std::string(title) == "Daytona Error" && !parent);
    ++dialogs; shown = message; return !fail_dialog;
}
}
int main() {
    check(app::create_gpu_device() != nullptr && created == 1 && destroyed == 1);
    for (const char *key : {SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
         SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN})
        check(properties.at(key));
    for (const char *key : {SDL_PROP_GPU_DEVICE_CREATE_FEATURE_CLIP_DISTANCE_BOOLEAN,
         SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN,
         SDL_PROP_GPU_DEVICE_CREATE_FEATURE_ANISOTROPY_BOOLEAN, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN})
        check(!properties.at(key));
    check(!properties.contains(SDL_PROP_GPU_DEVICE_CREATE_FEATURE_DEPTH_CLAMPING_BOOLEAN));
    fail_device = true;
    check(!app::create_gpu_device() && created == 2 && destroyed == 2);
    fail_property = true;
    check(!app::create_gpu_device() && created == 2 && destroyed == 3);
    fail_allocate = true;
    check(!app::create_gpu_device() && created == 2 && destroyed == 3);
    check(app::startup_error("SDL_CreateGPUDevice") == 1);
    check(shown == "SDL_CreateGPUDevice failed: No supported SDL_GPU backend found!" && dialogs == 1 && logs == 1);
    fail_dialog = true;
    check(app::startup_error("SDL_Init") == 1 && dialogs == 2 && logs == 3);
    std::puts("GPU startup properties, failure cleanup and visible error reporting passed");
}
