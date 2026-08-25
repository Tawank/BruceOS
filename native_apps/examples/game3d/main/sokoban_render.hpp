// sokoban_render.hpp -- Jet scene building, camera framing and HUD for the
// Sokoban clone in main.cpp. Everything here is a free function operating
// on caller-owned Objects/Materials and a GameState (sokoban_game.hpp);
// nothing here is a namespace-scope instance, per main.cpp's __dso_handle
// comment on this loader's C++ ABI constraints.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "core_sdk/display.h"

#include "Jet.hpp" // IWYU pragma: keep
#include "sokoban_game.hpp"

using namespace Renderer;

namespace {

// ---------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t kBackColor      = rgb565(16, 18, 28);
constexpr uint16_t kFloorColor     = rgb565(70, 76, 96);
constexpr uint16_t kGoalColor      = rgb565(150, 122, 40);
constexpr uint16_t kWallColor      = rgb565(58, 52, 66);
constexpr uint16_t kPlayerColor    = rgb565(60, 190, 225);
constexpr uint16_t kNoseColor      = rgb565(255, 235, 130);
constexpr uint16_t kBoxColor       = rgb565(196, 122, 45);
constexpr uint16_t kBoxOnGoalColor = rgb565(80, 210, 100);

// ---------------------------------------------------------------------
// World-scale constants (world units, same rough scale as the original
// spinning-cube demo's cubeSize -- see JetConfig.hpp).
// ---------------------------------------------------------------------

constexpr int32_t kTile     = 44; // grid pitch
constexpr int32_t kFloorGap = 4;  // gap between floor tiles, purely cosmetic
constexpr int32_t kWallH    = 40;
constexpr int32_t kBoxSize  = 30;
constexpr int32_t kPlayerW  = 22;
constexpr int32_t kPlayerH  = 30;
constexpr int32_t kNoseSize = 14;
constexpr int32_t kNoseOffset = kPlayerW / 2 + kNoseSize / 2 + 2;

inline int32_t cellWorldX(int c, int cols) { return (2 * c - (cols - 1)) * (kTile / 2); }
inline int32_t cellWorldZ(int r, int rows) { return (2 * r - (rows - 1)) * (kTile / 2); }

// Appends one axis-aligned box's faces (per-face normals) to `obj`,
// centred at (cx, cy, cz). Mirrors Primitives::createDebugCube's vertex
// layout so winding/culling matches the rest of the engine, just
// parameterised by a centre offset and pushed into a caller-owned Object
// instead of a fresh one -- lets many grid cells share one Object (one
// culling test, one draw walk) instead of one Object per tile.
//
// `includeBottom` drops the -Y face (24 verts -> 20) when the caller
// knows it can never be seen -- true for every wall in this level (the
// camera never gets under one) and worth doing given how memory-tight
// this loader's freestanding allocator is (see
// createStaticGeometryObjects()'s comment below).
inline void addBoxFaces(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t hw, int32_t hh, int32_t hd,
                         Material *mat, bool includeBottom = true) {
    const int32_t N = FIXED_POINT_SCALE;
    uint16_t b = (uint16_t)obj->vertices.size();

    // Front (+Z)
    obj->addVertex({{cx - hw, cy - hh, cz + hd}, {0, 0}, {0, 0, N}});
    obj->addVertex({{cx + hw, cy - hh, cz + hd}, {0, 0}, {0, 0, N}});
    obj->addVertex({{cx + hw, cy + hh, cz + hd}, {0, 0}, {0, 0, N}});
    obj->addVertex({{cx - hw, cy + hh, cz + hd}, {0, 0}, {0, 0, N}});
    // Back (-Z)
    obj->addVertex({{cx + hw, cy - hh, cz - hd}, {0, 0}, {0, 0, -N}});
    obj->addVertex({{cx - hw, cy - hh, cz - hd}, {0, 0}, {0, 0, -N}});
    obj->addVertex({{cx - hw, cy + hh, cz - hd}, {0, 0}, {0, 0, -N}});
    obj->addVertex({{cx + hw, cy + hh, cz - hd}, {0, 0}, {0, 0, -N}});
    // Left (-X)
    obj->addVertex({{cx - hw, cy - hh, cz - hd}, {0, 0}, {-N, 0, 0}});
    obj->addVertex({{cx - hw, cy - hh, cz + hd}, {0, 0}, {-N, 0, 0}});
    obj->addVertex({{cx - hw, cy + hh, cz + hd}, {0, 0}, {-N, 0, 0}});
    obj->addVertex({{cx - hw, cy + hh, cz - hd}, {0, 0}, {-N, 0, 0}});
    // Right (+X)
    obj->addVertex({{cx + hw, cy - hh, cz + hd}, {0, 0}, {N, 0, 0}});
    obj->addVertex({{cx + hw, cy - hh, cz - hd}, {0, 0}, {N, 0, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz - hd}, {0, 0}, {N, 0, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz + hd}, {0, 0}, {N, 0, 0}});
    // Top (+Y)
    obj->addVertex({{cx - hw, cy + hh, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz - hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx - hw, cy + hh, cz - hd}, {0, 0}, {0, N, 0}});

    obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
    obj->addFace(b + 4, b + 5, b + 6, b + 7, mat);
    obj->addFace(b + 8, b + 9, b + 10, b + 11, mat);
    obj->addFace(b + 12, b + 13, b + 14, b + 15, mat);
    obj->addFace(b + 16, b + 17, b + 18, b + 19, mat);

    if (!includeBottom) return;
    // Bottom (-Y)
    obj->addVertex({{cx - hw, cy - hh, cz - hd}, {0, 0}, {0, -N, 0}});
    obj->addVertex({{cx + hw, cy - hh, cz - hd}, {0, 0}, {0, -N, 0}});
    obj->addVertex({{cx + hw, cy - hh, cz + hd}, {0, 0}, {0, -N, 0}});
    obj->addVertex({{cx - hw, cy - hh, cz + hd}, {0, 0}, {0, -N, 0}});
    obj->addFace(b + 20, b + 21, b + 22, b + 23, mat);
}

// Appends a single flat +Y-facing quad (4 verts) at height `cy`, centred
// at (cx, cz) -- the top face of addBoxFaces() above, standalone. Floor
// tiles use this instead of a full box: their sides are never visible
// (nothing sits below y=0), so a 6-face box would spend 20 extra verts
// per tile for geometry that never draws -- a real cost given how many
// floor tiles a level has and how tight this loader's heap is (see
// createStaticGeometryObjects()'s comment below).
inline void addTopQuad(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t hw, int32_t hd, Material *mat) {
    const int32_t N = FIXED_POINT_SCALE;
    uint16_t b = (uint16_t)obj->vertices.size();
    obj->addVertex({{cx - hw, cy, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy, cz - hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx - hw, cy, cz - hd}, {0, 0}, {0, N, 0}});
    obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
}

// Creates the floor/wall Objects ONCE for the app's whole lifetime.
// Levels are then re-rendered by clearing and repopulating these same two
// Objects in place (rebuildLevelGeometry() below) instead of deleting and
// reallocating them per level.
//
// This matters on this loader specifically: the freestanding
// operator new it links against (elf_loader_sdk_symbols.c) returns NULL
// on failure instead of throwing, and nothing downstream -- including
// libstdc++'s own vector growth path -- checks for that, so a transient
// allocation failure turns into a wild pointer write instead of a clean
// abort. Never freeing this mesh data at all (just reserve()-ing it to
// exactly what the incoming level needs, see rebuildLevelGeometry) avoids
// both repeated free/realloc churn AND ever asking for more than the
// current level actually uses -- important because this loader's process
// heap is small enough that a naive "reserve for the worst-case level up
// front" (a fully-boxed floor+wall mesh can be several hundred KB) can by
// itself exceed what's available.
inline void createStaticGeometryObjects(Scene &scene, Object *&floorObj, Object *&wallObj) {
    floorObj = new Object();
    wallObj = new Object();
    scene.addObject(floorObj);
    scene.addObject(wallObj);
}

// Player, its facing "nose" cube, and kMaxBoxes box cubes -- created once
// up front like the static geometry above. Levels with fewer than
// kMaxBoxes boxes just leave the extra slots disabled (see
// applyLevelToActors()) instead of the Objects being created/destroyed
// per level.
inline void createDynamicObjects(Scene &scene, Material *playerMat, Material *noseMat, Material *boxMat[],
                                  Object *&playerObj, Object *&noseObj, Object *boxObj[]) {
    playerObj = Primitives::createCube(kPlayerW, kPlayerH, kPlayerW, playerMat);
    scene.addObject(playerObj);
    noseObj = Primitives::createCube(kNoseSize, kNoseSize, kNoseSize, noseMat);
    scene.addObject(noseObj);

    for (int i = 0; i < kMaxBoxes; i++) {
        boxObj[i] = Primitives::createCube(kBoxSize, kBoxSize, kBoxSize, boxMat[i]);
        scene.addObject(boxObj[i]);
    }
}

// Re-fills the floor/wall Objects with the current level's tiles: floor
// cells get a single flat top quad each (see addTopQuad()), wall cells a
// bottomless box (see addBoxFaces()). Counts cells first so vertices/
// triangles can be reserve()d to exactly what this level needs -- capacity
// only ever grows to the largest level seen so far, and never further
// than that, since reserve() is a no-op when already big enough.
inline void rebuildLevelGeometry(const GameState &gs, Material *floorMat, Material *goalMat, Material *wallMat,
                                  Object *floorObj, Object *wallObj) {
    int floorCells = 0, wallCells = 0;
    for (int r = 0; r < gs.rows; r++) {
        for (int c = 0; c < gs.cols; c++) {
            if (gs.cell[r][c] == CELL_WALL) wallCells++;
            else if (gs.cell[r][c] == CELL_FLOOR) floorCells++;
        }
    }

    floorObj->vertices.clear();
    floorObj->triangles.clear();
    wallObj->vertices.clear();
    wallObj->triangles.clear();
    floorObj->vertices.reserve((size_t)floorCells * 4);
    floorObj->triangles.reserve((size_t)floorCells * 2);
    wallObj->vertices.reserve((size_t)wallCells * 20); // addBoxFaces(includeBottom=false)
    wallObj->triangles.reserve((size_t)wallCells * 10);

    constexpr int32_t wallHalf = kTile / 2;
    constexpr int32_t floorHalf = (kTile - kFloorGap) / 2;

    for (int r = 0; r < gs.rows; r++) {
        for (int c = 0; c < gs.cols; c++) {
            if (gs.cell[r][c] == CELL_VOID) continue;
            int32_t x = cellWorldX(c, gs.cols);
            int32_t z = cellWorldZ(r, gs.rows);
            if (gs.cell[r][c] == CELL_WALL) {
                addBoxFaces(wallObj, x, kWallH / 2, z, wallHalf, kWallH / 2, wallHalf, wallMat,
                            /*includeBottom=*/false);
            } else {
                Material *mat = gs.goal[r][c] ? goalMat : floorMat;
                addTopQuad(floorObj, x, 0, z, floorHalf, floorHalf, mat);
            }
        }
    }

    floorObj->calculateBoundingBox();
    wallObj->calculateBoundingBox();
}

// Snaps player/nose/box visuals straight to their logical grid cells, no
// animation -- used right after (re)building a level. Box slots beyond
// this level's boxCount are disabled rather than repositioned.
inline void applyLevelToActors(const GameState &gs, Object *playerObj, Object *noseObj, Object *boxObj[],
                                Material *boxMat[], int facingDr, int facingDc) {
    int32_t px = cellWorldX(gs.playerC, gs.cols);
    int32_t pz = cellWorldZ(gs.playerR, gs.rows);
    constexpr int32_t py = kPlayerH / 2;
    playerObj->setPosition(px, py, pz);
    noseObj->setPosition(px + facingDc * kNoseOffset, py, pz + facingDr * kNoseOffset);

    for (int i = 0; i < kMaxBoxes; i++) {
        if (i >= gs.boxCount) {
            boxObj[i]->enabled = false;
            continue;
        }
        boxObj[i]->enabled = true;
        bool onGoal = gs.goal[gs.boxR[i]][gs.boxC[i]];
        boxMat[i]->color = onGoal ? kBoxOnGoalColor : kBoxColor;
        int32_t bx = cellWorldX(gs.boxC[i], gs.cols);
        int32_t bz = cellWorldZ(gs.boxR[i], gs.rows);
        boxObj[i]->setPosition(bx, kBoxSize / 2, bz);
    }
}

// Places the camera on the board's XZ diagonal, far enough back that the
// whole board (plus wall height) fits within `targetApparentRadius`
// pixels of screen radius, then points it at the board centre via
// lookAt() -- see main.cpp's top comment for why this fakes isometric on
// a perspective-only rasteriser. `fovFactor` (screenWidth/2 / tan(fov/2))
// is cached on Camera by setFOV(), so the apparent-size solve is just
// apparentRadius = fovFactor * worldRadius / distance, inverted for
// distance.
inline void frameCameraForBoard(Camera &camera, int rows, int cols, int width, int height) {
    camera.setFOV(50.0f, static_cast<int32_t>(width));

    int32_t boardHalfW = (cols * kTile) / 2;
    int32_t boardHalfD = (rows * kTile) / 2;
    float boardRadius = std::sqrt((float)boardHalfW * (float)boardHalfW + (float)boardHalfD * (float)boardHalfD +
                                   (float)kWallH * (float)kWallH);

    float targetApparentRadius = 0.40f * (float)std::min(width, height);
    if (targetApparentRadius < 1.0f) targetApparentRadius = 1.0f;

    float dist = camera.fovFactor * boardRadius / targetApparentRadius;
    float s = dist / 1.7320508f; // camera offset (s,s,s) has length s*sqrt(3)
    int32_t offset = (int32_t)s;
    if (offset < kTile) offset = kTile; // guard tiny/degenerate boards

    Vector3 target(0, kWallH / 2, 0);
    camera.setPosition(target.x + offset, target.y + offset, target.z - offset);
    camera.lookAt(target);

    camera.nearPlane = 32;
    camera.farPlane = (int32_t)(offset * 3.5f) + kTile * 4;
}

inline void drawHud(const GameState &gs, int levelIndex, int width, int height) {
    char line[48];
    std::snprintf(line, sizeof(line), "Level %d/%d   Moves %d", levelIndex + 1, kLevelCount, gs.moves);

    display__fill_rect(0, 0, (int16_t)width, 16, rgb565(8, 9, 14));
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    display__draw_string(line, 4, 4);

    if (gs.solved) {
        display__fill_rect(0, 18, (int16_t)width, 16, rgb565(18, 58, 22));
        display__set_text_color(rgb565(255, 235, 130));
        display__draw_centre_string("SOLVED! SELECT for next level", (int16_t)(width / 2), 22);
    } else {
        display__fill_rect(0, (int16_t)(height - 16), (int16_t)width, 16, rgb565(8, 9, 14));
        display__set_text_color(rgb565(150, 158, 180));
        display__draw_centre_string("SELECT restart  BACK exit", (int16_t)(width / 2), (int16_t)(height - 13));
    }
}

} // namespace
