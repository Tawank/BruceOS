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

// Draws the full-size rendered scene shrunk into a smaller viewport (the
// process preview tile), nearest-neighbour, one row at a time through `row`
// so no second framebuffer is needed. Fitted inside the viewport at the
// scene's own aspect ratio and letterboxed on whichever axis is left over,
// using the scene's corner pixel (always background) for the bars; the
// caller draws the HUD bars around it.
void drawSceneScaled(const uint16_t *scene, int sceneW, int sceneH, uint16_t *row, int viewY, int viewW, int viewH) {
    int drawW = viewW;
    int drawH = sceneH * viewW / sceneW;
    if (drawH > viewH) {
        drawH = viewH;
        drawW = sceneW * viewH / sceneH;
    }
    if (drawW < 1) drawW = 1;
    if (drawH < 1) drawH = 1;
    const int left = (viewW - drawW) / 2;
    const int top = (viewH - drawH) / 2;
    for (int y = 0; y < viewH; y++) {
        for (int x = 0; x < viewW; x++) row[x] = scene[0];
        if (y >= top && y < top + drawH) {
            const uint16_t *src = scene + (size_t)((y - top) * sceneH / drawH) * (size_t)sceneW;
            for (int x = 0; x < drawW; x++) row[left + x] = src[x * sceneW / drawW];
        }
        display__draw_rgb_bitmap(0, (int16_t)(viewY + y), row, (int16_t)viewW, 1);
    }
}
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

    // Jet's ESP32-S3 clear writes rows with 128-bit vector stores, which
    // silently round the address down to 16 bytes. memory__malloc() only
    // guarantees 4-byte alignment (its header is 20 bytes), so an unaligned
    // buffer has row 0 written over the allocation header in front of it -
    // corrupting the heap once the buffer is freed. Over-allocate and align.
    constexpr uintptr_t kFramebufferAlign = 16;
    uint16_t *framebufferAllocation =
        new uint16_t[(size_t)width * (size_t)gameHeight + kFramebufferAlign / sizeof(uint16_t)];
    if (framebufferAllocation == nullptr) {
        printf("game3d: framebuffer allocation failed (%dx%d)\n", width, gameHeight);
        return 1;
    }
    uint16_t *framebuffer = reinterpret_cast<uint16_t *>(
        (reinterpret_cast<uintptr_t>(framebufferAllocation) + kFramebufferAlign - 1) & ~(kFramebufferAlign - 1));

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

    // All Materials and Objects are created exactly once here, up front,
    // and reused for every level for the rest of the app's run -- see
    // createStaticGeometryObjects()'s comment in sokoban_render.hpp for
    // why level transitions never delete/reallocate any of these.
    //
    // floor/goal/wall/player/box/beak are ShadedMaterial triples (top/lit/
    // shadow tones baked from one base colour, sokoban_geometry.hpp) so
    // they read as lit from one fake-sun direction instead of flat;
    // accent stays a single flat Material -- too small (eyes, feet, crate
    // straps) for directional shading to read.
    ShadedMaterial floorMat, goalMat, wallMat, playerMat, beakMat, boxMat[kMaxBoxes];
    bool matsOk = makeShadedMaterial(floorMat, kFloorColor);
    matsOk &= makeShadedMaterial(goalMat, kGoalColor);
    matsOk &= makeShadedMaterial(wallMat, kWallColor);
    matsOk &= makeShadedMaterial(playerMat, kPlayerColor);
    matsOk &= makeShadedMaterial(beakMat, kBeakColor);
    for (int i = 0; i < kMaxBoxes; i++) matsOk &= makeShadedMaterial(boxMat[i], kBoxColor);
    Material *accentMat = new Material(kAccentColor); // eyes, feet, crate straps

    // operator new returns NULL on failure here instead of throwing (see
    // sokoban_render.hpp), and nothing downstream checks that -- check it
    // ourselves rather than let a NULL Material* become a wild write.
    if (!matsOk || !accentMat) {
        printf("game3d: material allocation failed\n");
        logHeap("material allocation failed");
        delete[] framebufferAllocation;
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
        delete[] framebufferAllocation;
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

    // Faces the player along (dr, dc) where it stands, e.g. a turn or a blocked move.
    auto turnTo = [&](int dr, int dc) {
        facingDr = dr;
        facingDc = dc;
        rebuildFaceGeometry(faceObj, beakMat, accentMat, facingDr, facingDc);
        faceObj->setPosition(cellWorldX(gs.playerC, gs.cols), kPlayerH / 2, cellWorldZ(gs.playerR, gs.rows));
        dirty = true;
    };

    // One scanline for drawSceneScaled(); without it the preview tile just stays blank.
    uint16_t *previewRow = new uint16_t[width];

    bool hudCleared = true;
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

            // The keyboard's Enter shares SELECT's code; only its source tells them apart.
            bool enterKey = ev.code == BRUCE_INPUT_CODE_SELECT && ev.type == BRUCE_INPUT_KEY;
            if (enterKey || (gs.solved && ev.code == BRUCE_INPUT_CODE_SELECT)) {
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
                case BRUCE_INPUT_CODE_NEXT: turnTo(-facingDc, facingDr); continue; // clockwise
                case BRUCE_INPUT_CODE_PREV: turnTo(facingDc, -facingDr); continue;
                case BRUCE_INPUT_CODE_SELECT:
                    dr = facingDr;
                    dc = facingDc;
                    break;
                default: continue;
            }

            turnTo(dr, dc);
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

        // Tiled into the process preview, back in the foreground, or moved:
        // Core cleared our viewport, so the scene has to be drawn again.
        if (display__viewport_changed()) {
            dirty = true;
            hudCleared = true;
        }

        // A backgrounded app that isn't tiled has no viewport (0x0); keep the
        // frame pending until it is visible again.
        const int viewW = display__width();
        const int viewH = display__height();
        if (dirty && viewW > 0 && viewH > 0) {
            scene.render();
            if (display__begin_frame() == BRUCE_OK) {
                if (viewW == width && viewH == height) {
                    display__draw_rgb_bitmap(0, (int16_t)gameY, framebuffer, (int16_t)width, (int16_t)gameHeight);
                    drawHud(gs, levelIndex, width, height, hudCleared);
                } else if (previewRow != nullptr && viewW <= width) {
                    const int hudH = viewH > 3 * kHudBarH ? kHudBarH : 0;
                    drawSceneScaled(framebuffer, width, gameHeight, previewRow, hudH, viewW, viewH - 2 * hudH);
                    if (hudH > 0) drawHud(gs, levelIndex, viewW, viewH, true);
                }
                display__present();
            }
            dirty = false;
            hudCleared = false;
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
    deleteShadedMaterial(beakMat);
    delete accentMat;
    for (int i = 0; i < kMaxBoxes; i++) deleteShadedMaterial(boxMat[i]);
    delete[] previewRow;
    delete[] framebufferAllocation;
    printf("game3d exiting\n");
    return 0;
}
