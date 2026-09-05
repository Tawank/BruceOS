// Game3D: an isometric 3D Sokoban clone driving components/jet, a small
// fixed-function 3D rasteriser (https://github.com/CubeCoders/Jet). See
// native_apps/examples/game3d and components/jet's READMEs for the
// C++-on-ELF-loader story this app depends on -- in short: no exceptions,
// no RTTI, no global constructors.
//
// The level, the push-box rules and the win check are ordinary 2D grid
// logic (sokoban_levels.hpp / sokoban_game.hpp); sokoban_render.hpp and
// sokoban_actors.hpp turn a GameState into Jet Objects and this file just
// drives the input/animation loop. The camera follows the player -- a
// fixed XZ-diagonal offset from its current world position, re-aimed every
// frame (frameCameraOnPlayer(), sokoban_render.hpp) -- the classic "fake
// isometric" trick (camera pulled far back on a 45/35.264-degree diagonal)
// applied around a moving point.

#include <cstdint>
#include <cstdio>

#include "core_sdk/display.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/runtime.h"

#include "sokoban_actors.hpp"
#include "sokoban_game.hpp"
#include "sokoban_render.hpp"

// __dso_handle: ABI-mandated __cxa_atexit destructor-registration tag
// (Scene.cpp's function-local `static std::vector<...>`s need one). Hidden
// ELF visibility by convention, so it must be defined in this app's own
// linked objects rather than resolved by the loader; never dereferenced,
// only compared for identity -- "points at itself" (as libc's crtstuff.c
// does) is enough. Any C++ ELF app with a function-local static that has a
// non-trivial destructor will need this same line.
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

    // Hand back Core's off-screen framebuffer RAM before allocating our own
    // colour buffer below: there's no API to draw straight into Core's
    // buffer (apps are isolated from it, same as any other core/ internal),
    // so this app always needs its own width*gameHeight*2 bytes regardless -
    // requesting DIRECT mode just guarantees Core's copy is freed rather
    // than leaving it allocated-and-unused alongside ours. This is on top
    // of (not a replacement for) the ELF loader's automatic
    // memory__reclaim() ahead of app_main(), which only drops Core into
    // DIRECT if the manifest's declared heap/stack size didn't already fit -
    // a request here covers the common case where it did fit but this
    // buffer still doesn't need to coexist with Core's. Released
    // automatically when this process exits (display__request_render_mode()),
    // so the display returns to whatever mode the remaining live requests
    // (if any) call for - same as process_app's tile preview, which depends
    // on Core's shared framebuffer and would have nowhere to composite into
    // while this request is held; this game is a full-screen foreground
    // app, so that trade-off is acceptable while it's running.
    if (display__request_render_mode(BRUCE_DISPLAY_MODE_DIRECT) != BRUCE_OK) {
        printf("game3d: direct render mode request failed, continuing buffered\n");
    }

    initializeTrigTables();

    // The 3D scene is rendered into a shorter buffer than the physical
    // screen, leaving a dedicated row of screen space free above and below
    // for drawHud()'s bars (sokoban_render.hpp) -- they're drawn straight
    // to the display, never through this buffer, so they need their own
    // rows the scene blit will never overwrite.
    int gameHeight = height - 2 * kHudBarH;
    int gameY = kHudBarH;
    if (gameHeight < 1) {
        gameHeight = height;
        gameY = 0;
    }

    uint16_t *framebuffer = new uint16_t[(size_t)width * (size_t)gameHeight];
    if (framebuffer == nullptr) {
        printf("game3d: framebuffer allocation failed (%dx%d)\n", width, gameHeight);
        return 1;
    }

    // Diagnostic only: logged at each major allocation step (and later,
    // each move) so a hardware crash/hang tells us which step ran out of
    // room instead of just where the resulting wild pointer write happened.
    auto logHeap = [](const char *tag) {
        bruce_memory_stats_t s;
        if (memory__get_stats(&s) == BRUCE_OK) {
            printf("game3d: heap free=%u largest_block=%u (%s)\n", (unsigned)s.internal_free,
                   (unsigned)s.internal_largest_block, tag);
        }
    };
    logHeap("after framebuffer alloc");

    // Z_BUFFERING is off (see JetConfig.hpp), so no depth buffer -- Scene
    // accepts nullptr. SORT_TRIANGLES (painter's algorithm) suffices:
    // everything here is opaque with a distinct view-space depth.
    Scene scene(framebuffer, nullptr, width, gameHeight);
    scene.setBackcolor(kBackColor);

    Camera camera;
    scene.setCamera(&camera);

    // Pre-size Scene's internal render queue to this app's worst-case
    // per-frame triangle count -- see Scene::reserveRenderQueue()'s own
    // comment (components/jet's Scene.hpp/.cpp) for why, and do it now,
    // before a single Object or Material exists, while the heap is at its
    // least fragmented.
    scene.reserveRenderQueue((size_t)worstCaseFrameTriangles());
    logHeap("after renderQueue reserve");

    // All Materials and Objects are created exactly once here, up front,
    // and reused for every level for the rest of the app's run -- see
    // createStaticGeometryObjects()'s comment in sokoban_render.hpp for
    // why level transitions never delete/reallocate any of these.
    //
    // floor/goal/wall/player/box are ShadedMaterial triples (top/lit/shadow
    // tones baked from one base colour, sokoban_geometry.hpp) so their
    // boxes read as lit from one fake-sun direction instead of flat;
    // beak/accent stay single flat Materials -- too small (eyes, feet,
    // crate straps) for directional shading to read.
    ShadedMaterial floorMat, goalMat, wallMat, playerMat, boxMat[kMaxBoxes];
    bool matsOk = makeShadedMaterial(floorMat, kFloorColor);
    matsOk &= makeShadedMaterial(goalMat, kGoalColor);
    matsOk &= makeShadedMaterial(wallMat, kWallColor);
    matsOk &= makeShadedMaterial(playerMat, kPlayerColor);
    for (int i = 0; i < kMaxBoxes; i++) matsOk &= makeShadedMaterial(boxMat[i], kBoxColor);
    Material *beakMat = new Material(kBeakColor);
    Material *accentMat = new Material(kAccentColor); // eyes, feet, crate straps

    // operator new returns NULL on failure here instead of throwing (see
    // sokoban_render.hpp), and nothing downstream checks that -- check it
    // ourselves rather than let a NULL Material* become a wild write.
    if (!matsOk || !beakMat || !accentMat) {
        printf("game3d: material allocation failed\n");
        logHeap("material allocation failed");
        delete[] framebuffer;
        return 1;
    }
    logHeap("after materials");

    Object *floorObj = nullptr, *wallObj = nullptr;
    createStaticGeometryObjects(scene, floorObj, wallObj);
    reserveWorstCaseLevelGeometry(floorObj, wallObj);
    logHeap("after static geometry objects");
    Object *playerObj = nullptr, *faceObj = nullptr;
    Object *boxObj[kMaxBoxes] = {};
    createDynamicObjects(scene, playerMat, accentMat, boxMat, playerObj, faceObj, boxObj);
    logHeap("after dynamic objects (player/face/boxes)");

    if (!floorObj || !wallObj || !playerObj || !faceObj || !boxObj[0] || !boxObj[1] || !boxObj[2] || !boxObj[3]) {
        printf("game3d: object allocation failed\n");
        delete[] framebuffer;
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
        applyLevelToActors(gs, playerObj, faceObj, boxObj, boxMat, beakMat, accentMat, facingDr, facingDc);
        frameCameraOnPlayer(camera, cellWorldX(gs.playerC, gs.cols), cellWorldZ(gs.playerR, gs.rows), width,
                             gameHeight);
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
        // Standard event-loop drain (see native_apps/examples/nes's
        // nes_input.c) instead of input__check()-per-code polling: pulls
        // every currently-queued event once via input__poll() and switches
        // on its code, so nothing can accumulate in the queue between
        // frames regardless of which codes this loop happens to test for.
        bruce_input_event_t ev;
        while (input__poll(&ev) == BRUCE_OK) {
            if (ev.action != BRUCE_INPUT_PRESS) continue;
            if (ev.code == BRUCE_INPUT_CODE_BACK) {
                running = false;
                continue;
            }
            if (animActive) continue; // mid-slide: drop everything else

            if (ev.code == BRUCE_INPUT_CODE_SELECT) {
                if (gs.solved) levelIndex = (levelIndex + 1) % kLevelCount;
                reloadLevel(levelIndex); // also serves as "restart" when not solved
                continue;
            }
            if (gs.solved) continue; // ignore stray direction presses

            int dr = 0, dc = 0;
            switch (ev.code) {
                case BRUCE_INPUT_CODE_UP: dr = 1; break;
                case BRUCE_INPUT_CODE_DOWN: dr = -1; break;
                case BRUCE_INPUT_CODE_LEFT: dc = -1; break;
                case BRUCE_INPUT_CODE_RIGHT: dc = 1; break;
                default: continue;
            }

            facingDr = dr;
            facingDc = dc;
            rebuildFaceGeometry(faceObj, beakMat, accentMat, facingDr, facingDc);
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
                    // The box started in the cell the player just moved into.
                    animBX0 = animPX1;
                    animBZ0 = animPZ1;
                    animBX1 = cellWorldX(gs.boxC[animBoxIdx], gs.cols);
                    animBZ1 = cellWorldZ(gs.boxR[animBoxIdx], gs.rows);
                    bool onGoal = gs.goal[gs.boxR[animBoxIdx]][gs.boxC[animBoxIdx]];
                    setShadedColor(boxMat[animBoxIdx], onGoal ? kBoxOnGoalColor : kBoxColor);
                }
                animActive = true;
                animFrame = 0;
            } else {
                // Blocked: still snap the face to the direction that was
                // tried (rebuildFaceGeometry() above already did the
                // shape), no animation needed.
                int32_t px = cellWorldX(gs.playerC, gs.cols);
                int32_t pz = cellWorldZ(gs.playerR, gs.rows);
                faceObj->setPosition(px, kPlayerH / 2, pz);
                dirty = true;
            }
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
            faceObj->setPosition(px, kPlayerH / 2, pz);
            frameCameraOnPlayer(camera, px, pz, width, gameHeight);

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
                display__draw_rgb_bitmap(0, (int16_t)gameY, framebuffer, (int16_t)width, (int16_t)gameHeight);
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
    delete faceObj;
    for (int i = 0; i < kMaxBoxes; i++) delete boxObj[i];
    deleteShadedMaterial(floorMat);
    deleteShadedMaterial(goalMat);
    deleteShadedMaterial(wallMat);
    deleteShadedMaterial(playerMat);
    delete beakMat;
    delete accentMat;
    for (int i = 0; i < kMaxBoxes; i++) deleteShadedMaterial(boxMat[i]);
    delete[] framebuffer;
    printf("game3d exiting\n");
    return 0;
}
