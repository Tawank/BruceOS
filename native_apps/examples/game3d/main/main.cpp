// Game3D: a minimal native ELF example driving components/jet, a small
// fixed-function 3D rasteriser (https://github.com/CubeCoders/Jet), to spin
// a flat-shaded debug cube on the display. See native_apps/examples/game3d
// and components/jet's READMEs for the C++-on-ELF-loader story this app
// depends on -- in short: no exceptions, no RTTI, no global constructors.
//
// Loosely modelled on native_apps/examples/game (renamed/replaced here) and
// on Jet's own upstream Sample.cpp, adapted to Bruce's core_sdk display API
// instead of Jet's own mbed sample harness.

#include <cstdio>

#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/runtime.h"

#include "Jet.hpp"

using namespace Renderer;

// __dso_handle: an ABI-mandated tag identifying which "shared object" a
// __cxa_atexit-registered destructor belongs to (Scene.cpp has function-
// local `static std::vector<...>`s, which need one -- see
// elf_loader_sdk_symbols.c's __cxa_atexit stub for why the registration
// itself is a safe no-op here). Unlike every other symbol that stub file
// resolves at load time, __dso_handle has hidden ELF visibility by
// convention, so project_elf()'s -fPIC -shared link demands it be defined
// inside this app's own linked objects -- it cannot be deferred to the
// runtime loader. Its value is never dereferenced, only compared for
// identity, so the standard "points at itself" definition (the same one
// libc's own crtstuff.c uses) is all that's needed. Any other C++ ELF app
// hitting the same link error (function-local static with a non-trivial
// destructor) will need this same line.
extern "C" {
void *__dso_handle = (void *)&__dso_handle;
}

namespace {

// Degrees per frame for each axis; arbitrary, just enough to show all six
// faces over time.
constexpr int32_t kSpinX = 1;
constexpr int32_t kSpinY = 2;
constexpr int32_t kSpinZ = 0;

// One render/present cycle. Not vsynced to the panel -- this fixed delay is
// a simple, portable pace across the boards this app targets rather than a
// measured frame budget (contrast with native_apps/examples/nes/main/
// nes_video.c, which paces against the emulated console's real refresh
// rate).
constexpr uint32_t kFrameDelayMs = 16;

} // namespace

extern "C" int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("game3d started\n");

    const int width = display__width();
    const int height = display__height();
    if (width <= 0 || height <= 0) {
        printf("game3d: no viewport, exiting\n");
        return 0;
    }

    // Hand back the buffered/DMA framebuffer's RAM for the duration of this
    // app, the same handoff native_apps/examples/nes uses -- Jet's own
    // colour buffer below needs width*height*2 bytes in addition to it.
    display__game_mode(true);

    initializeTrigTables();

    uint16_t *framebuffer = new uint16_t[(size_t)width * (size_t)height];
    if (framebuffer == nullptr) {
        printf("game3d: framebuffer allocation failed (%dx%d)\n", width, height);
        display__game_mode(false);
        return 1;
    }

    // Z_BUFFERING is off in JetConfig.hpp (see its comment for why), so no
    // depth buffer is allocated -- Scene accepts nullptr for that case.
    Scene scene(framebuffer, nullptr, width, height);
    scene.setBackcolor(BRUCE_COLOR_BLACK);

    Camera camera;
    camera.setPosition(0, 0, -600);
    // Explicit int32_t casts: Camera::setFOV() is overloaded on
    // (int32_t, int32_t) vs (float, int32_t), and int32_t is a distinct
    // type from int on this toolchain (not just another name for it), so a
    // bare `60` is an equally-good conversion target for both overloads --
    // ambiguous without the cast.
    camera.setFOV(static_cast<int32_t>(60), static_cast<int32_t>(width));
    scene.setCamera(&camera);

    // 6 faces, one flat colour each -- see components/jet/src/Primitives.cpp.
    // Deliberately not using lighting (JetConfig.hpp: LIGHTING 0): the debug
    // cube's own per-face colours are the whole point, an easy way to see
    // the object's orientation as it spins.
    int32_t cubeSize = width < height ? width / 2 : height / 2;
    if (cubeSize < 16) cubeSize = 16;
    Object *cube = Primitives::createDebugCube(cubeSize, cubeSize, cubeSize);
    scene.addObject(cube);

    bool running = true;
    while (running) {
        cube->rotate(kSpinX, kSpinY, kSpinZ);
        scene.render();

        if (display__begin_frame() == BRUCE_OK) {
            display__draw_rgb_bitmap(0, 0, framebuffer, (int16_t)width, (int16_t)height);
            display__present();
        }

        if (input__check(BRUCE_INPUT_CODE_BACK, true)) running = false;

        runtime__delay(kFrameDelayMs);
    }

    delete cube;
    delete[] framebuffer;
    display__game_mode(false);
    printf("game3d exiting\n");
    return 0;
}
