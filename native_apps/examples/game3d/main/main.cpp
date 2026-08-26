// Game3D: an isometric 3D Sokoban clone driving components/jet, a small
// fixed-function 3D rasteriser (https://github.com/CubeCoders/Jet). See
// native_apps/examples/game3d and components/jet's READMEs for the
// C++-on-ELF-loader story this app depends on -- in short: no exceptions,
// no RTTI, no global constructors.
//
// The level, the push-box rules and the win check are ordinary 2D grid
// logic (sokoban_levels.hpp / sokoban_game.hpp); sokoban_render.hpp turns a
// GameState into Jet Objects and this file just drives the input/animation
// loop. The camera follows the player: a fixed XZ-diagonal offset from its
// current world position, re-aimed every frame (frameCameraOnPlayer(),
// sokoban_render.hpp) so it pans to keep the cube centred and close
// through a move -- the classic "fake isometric" trick (pull the camera
// far back on a 45/35.264-degree diagonal) applied around a moving point
// instead of the board centre.

#include <cstdint>
#include <cstdio>

#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

#include "sokoban_game.hpp"
#include "sokoban_render.hpp"

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
constexpr int kAnimTotalFrames = 6;
constexpr uint32_t kAnimFrameDelayMs = 16;
constexpr uint32_t kIdleDelayMs = 30; // turn-based: no need to poll at 60fps while idle
} // namespace

extern "C" int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("game3d (sokoban) started\n");

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

    // Diagnostic only: not used for any decision here, just logged at each
    // major allocation step so a hardware crash tells us which step ran
    // out of room instead of just where the resulting wild pointer write
    // happened (see createStaticGeometryObjects()'s comment in
    // sokoban_render.hpp for why that headroom matters on this loader
    // specifically).
    auto logHeap = [](const char *tag) {
        bruce_memory_stats_t s;
        if (memory__get_stats(&s) == BRUCE_OK) {
            printf("game3d: heap free=%u largest_block=%u (%s)\n", (unsigned)s.internal_free,
                   (unsigned)s.internal_largest_block, tag);
        }
    };
    logHeap("after framebuffer alloc");

    // Z_BUFFERING is off in JetConfig.hpp (see its comment for why), so no
    // depth buffer is allocated -- Scene accepts nullptr for that case.
    // SORT_TRIANGLES (painter's algorithm) is enough here: every tile,
    // wall, box and the player are opaque, and the fixed diagonal camera
    // gives every triangle a distinct view-space depth.
    Scene scene(framebuffer, nullptr, width, height);
    scene.setBackcolor(kBackColor);

    Camera camera;
    scene.setCamera(&camera);

    // All Materials and Objects are created exactly once here, up front,
    // and reused for every level for the rest of the app's run -- see
    // createStaticGeometryObjects()'s comment in sokoban_render.hpp for
    // why level transitions never delete/reallocate any of these.
    Material *floorMat = new Material(kFloorColor);
    Material *goalMat = new Material(kGoalColor);
    Material *wallMat = new Material(kWallColor);
    Material *playerMat = new Material(kPlayerColor);
    Material *noseMat = new Material(kNoseColor);
    Material *boxMat[kMaxBoxes];
    for (int i = 0; i < kMaxBoxes; i++) boxMat[i] = new Material(kBoxColor);

    // operator new returns NULL on failure here instead of throwing (see
    // sokoban_render.hpp), and nothing downstream checks that -- check it
    // ourselves rather than let a NULL Material* become a wild write.
    if (!floorMat || !goalMat || !wallMat || !playerMat || !noseMat || !boxMat[0] || !boxMat[1] || !boxMat[2] || !boxMat[3]) {
        printf("game3d: material allocation failed\n");
        logHeap("material allocation failed");
        delete[] framebuffer;
        display__game_mode(false);
        return 1;
    }
    logHeap("after materials");

    Object *floorObj = nullptr, *wallObj = nullptr;
    createStaticGeometryObjects(scene, floorObj, wallObj);
    reserveWorstCaseLevelGeometry(floorObj, wallObj);
    logHeap("after static geometry objects");
    Object *playerObj = nullptr, *noseObj = nullptr;
    Object *boxObj[kMaxBoxes] = {};
    createDynamicObjects(scene, playerMat, noseMat, boxMat, playerObj, noseObj, boxObj);
    logHeap("after dynamic objects (player/nose/boxes)");

    if (!floorObj || !wallObj || !playerObj || !noseObj || !boxObj[0] || !boxObj[1] || !boxObj[2] || !boxObj[3]) {
        printf("game3d: object allocation failed\n");
        delete[] framebuffer;
        display__game_mode(false);
        return 1;
    }

    GameState gs;
    int facingDr = 1, facingDc = 0; // facing "south" by default

    int levelIndex = 0;
    bool dirty = true;

    auto reloadLevel = [&](int idx) {
        loadLevel(idx, gs);
        rebuildLevelGeometry(gs, floorMat, goalMat, wallMat, floorObj, wallObj);
        logHeap("after rebuildLevelGeometry");
        facingDr = 1;
        facingDc = 0;
        applyLevelToActors(gs, playerObj, noseObj, boxObj, boxMat, facingDr, facingDc);
        frameCameraOnPlayer(camera, cellWorldX(gs.playerC, gs.cols), cellWorldZ(gs.playerR, gs.rows), width, height);
        dirty = true;
    };
    reloadLevel(levelIndex);

    bool animActive = false;
    int animFrame = 0;
    int32_t animPX0 = 0, animPZ0 = 0, animPX1 = 0, animPZ1 = 0;
    bool animPushed = false;
    int animBoxIdx = -1;
    int32_t animBX0 = 0, animBZ0 = 0, animBX1 = 0, animBZ1 = 0;

    bool running = true;
    while (running) {
        if (input__check(BRUCE_INPUT_CODE_BACK, true)) running = false;

        if (!animActive) {
            if (input__check(BRUCE_INPUT_CODE_SELECT, true)) {
                if (gs.solved) levelIndex = (levelIndex + 1) % kLevelCount;
                reloadLevel(levelIndex); // also serves as "restart" when not solved
            } else if (!gs.solved) {
                int dr = 0, dc = 0;
                bool haveDir = false;
                if (input__check(BRUCE_INPUT_CODE_UP, true)) {
                    dr = 1;
                    haveDir = true;
                } else if (input__check(BRUCE_INPUT_CODE_DOWN, true)) {
                    dr = -1;
                    haveDir = true;
                } else if (input__check(BRUCE_INPUT_CODE_LEFT, true)) {
                    dc = -1;
                    haveDir = true;
                } else if (input__check(BRUCE_INPUT_CODE_RIGHT, true)) {
                    dc = 1;
                    haveDir = true;
                }

                if (haveDir) {
                    facingDr = dr;
                    facingDc = dc;
                    int oldR = gs.playerR, oldC = gs.playerC;
                    MoveResult res = tryMove(gs, dr, dc);
                    if (res.moved) {
                        animPX0 = cellWorldX(oldC, gs.cols);
                        animPZ0 = cellWorldZ(oldR, gs.rows);
                        animPX1 = cellWorldX(gs.playerC, gs.cols);
                        animPZ1 = cellWorldZ(gs.playerR, gs.rows);
                        animPushed = res.pushedBox;
                        animBoxIdx = res.boxIndex;
                        if (animPushed) {
                            // The box started in the cell the player just
                            // moved into.
                            animBX0 = animPX1;
                            animBZ0 = animPZ1;
                            animBX1 = cellWorldX(gs.boxC[animBoxIdx], gs.cols);
                            animBZ1 = cellWorldZ(gs.boxR[animBoxIdx], gs.rows);
                            bool onGoal = gs.goal[gs.boxR[animBoxIdx]][gs.boxC[animBoxIdx]];
                            boxMat[animBoxIdx]->color = onGoal ? kBoxOnGoalColor : kBoxColor;
                        }
                        animActive = true;
                        animFrame = 0;
                    } else {
                        // Blocked: still snap the facing nose to the
                        // direction that was tried, no animation needed.
                        int32_t px = cellWorldX(gs.playerC, gs.cols);
                        int32_t pz = cellWorldZ(gs.playerR, gs.rows);
                        noseObj->setPosition(
                            px + facingDc * kNoseOffset, kPlayerH / 2, pz + facingDr * kNoseOffset
                        );
                        dirty = true;
                    }
                }
            } else {
                // Solved: drop stray directional presses instead of
                // letting them queue up for the next level.
                input__check(BRUCE_INPUT_CODE_UP, true);
                input__check(BRUCE_INPUT_CODE_DOWN, true);
                input__check(BRUCE_INPUT_CODE_LEFT, true);
                input__check(BRUCE_INPUT_CODE_RIGHT, true);
            }
        } else {
            // Mid-slide: drop new move/restart presses rather than queuing
            // them for when the animation ends.
            input__check(BRUCE_INPUT_CODE_UP, true);
            input__check(BRUCE_INPUT_CODE_DOWN, true);
            input__check(BRUCE_INPUT_CODE_LEFT, true);
            input__check(BRUCE_INPUT_CODE_RIGHT, true);
            input__check(BRUCE_INPUT_CODE_SELECT, true);
        }

        if (animActive) {
            animFrame++;
            float t = (float)animFrame / (float)kAnimTotalFrames;
            if (t >= 1.0f) {
                t = 1.0f;
                animActive = false;
            }

            int32_t px = animPX0 + (int32_t)((float)(animPX1 - animPX0) * t);
            int32_t pz = animPZ0 + (int32_t)((float)(animPZ1 - animPZ0) * t);
            playerObj->setPosition(px, kPlayerH / 2, pz);
            noseObj->setPosition(px + facingDc * kNoseOffset, kPlayerH / 2, pz + facingDr * kNoseOffset);
            frameCameraOnPlayer(camera, px, pz, width, height);

            if (animPushed) {
                int32_t bx = animBX0 + (int32_t)((float)(animBX1 - animBX0) * t);
                int32_t bz = animBZ0 + (int32_t)((float)(animBZ1 - animBZ0) * t);
                boxObj[animBoxIdx]->setPosition(bx, kBoxSize / 2, bz);
            }
            dirty = true;
        }

        if (dirty) {
            scene.render();
            if (display__begin_frame() == BRUCE_OK) {
                display__draw_rgb_bitmap(0, 0, framebuffer, (int16_t)width, (int16_t)height);
                drawHud(gs, levelIndex, width, height);
                display__present();
            }
            dirty = false;
        }

        // Turn-based game: pace tightly while a slide animation is
        // playing, otherwise poll input at a much lighter rate instead of
        // spinning at a fixed frame rate with nothing changing on screen.
        runtime__delay(animActive ? kAnimFrameDelayMs : kIdleDelayMs);
    }

    delete floorObj;
    delete wallObj;
    delete playerObj;
    delete noseObj;
    for (int i = 0; i < kMaxBoxes; i++) delete boxObj[i];
    delete floorMat;
    delete goalMat;
    delete wallMat;
    delete playerMat;
    delete noseMat;
    for (int i = 0; i < kMaxBoxes; i++) delete boxMat[i];
    delete[] framebuffer;
    display__game_mode(false);
    printf("game3d exiting\n");
    return 0;
}
