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
#include "sokoban_geometry.hpp"

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

constexpr int32_t kTile     = 40; // grid pitch
constexpr int32_t kFloorGap = 3;  // gap between floor tiles, purely cosmetic
constexpr int32_t kWallH    = 40;
constexpr int32_t kIslandDepth = 26; // side-face depth for a wall-less level's exposed rim
constexpr int32_t kBoxSize  = 34; // objects sized close to the tile on purpose -- see frameCameraForBoard()
constexpr int32_t kPlayerW  = 26;
constexpr int32_t kPlayerH  = 34;
constexpr int32_t kNoseSize = 16;
constexpr int32_t kNoseOffset = kPlayerW / 2 + kNoseSize / 2 + 2;

inline int32_t cellWorldX(int c, int cols) { return (2 * c - (cols - 1)) * (kTile / 2); }
inline int32_t cellWorldZ(int r, int rows) { return (2 * r - (rows - 1)) * (kTile / 2); }

// Creates the floor/wall Objects ONCE for the app's whole lifetime; levels
// are re-rendered by clearing and repopulating these same two Objects in
// place (rebuildLevelGeometry() below), never deleted/reallocated per
// level. Matters because this loader's operator new returns NULL on
// failure instead of throwing and nothing downstream checks that -- a
// transient allocation failure becomes a wild pointer write, not a clean
// abort (see elf_loader_sdk_symbols.c).
inline void createStaticGeometryObjects(Scene &scene, Object *&floorObj, Object *&wallObj) {
    floorObj = new Object();
    wallObj = new Object();
    scene.addObject(floorObj);
    scene.addObject(wallObj);
}

// Builds one origin-centred cube via addBoxFaces() with its vectors
// reserve()d to their exact final size (24 verts, 12 tris) up front --
// unlike Primitives::createCube(), which push_backs 24 times unreserved,
// risking several separately-failable reallocations to get there.
inline Object *createReservedCube(Scene &scene, int32_t w, int32_t h, int32_t d, Material *mat) {
    Object *obj = new Object();
    obj->vertices.reserve(24);
    obj->triangles.reserve(12);
    addBoxFaces(obj, 0, 0, 0, w / 2, h / 2, d / 2, mat);
    obj->calculateBoundingBox();
    scene.addObject(obj);
    return obj;
}

// Player, its facing "nose" cube, and kMaxBoxes box cubes -- created once
// up front like the static geometry above. Levels with fewer than
// kMaxBoxes boxes just leave the extra slots disabled (see
// applyLevelToActors()) instead of the Objects being created/destroyed
// per level.
inline void createDynamicObjects(Scene &scene, Material *playerMat, Material *noseMat, Material *boxMat[],
                                  Object *&playerObj, Object *&noseObj, Object *boxObj[]) {
    playerObj = createReservedCube(scene, kPlayerW, kPlayerH, kPlayerW, playerMat);
    noseObj = createReservedCube(scene, kNoseSize, kNoseSize, kNoseSize, noseMat);
    for (int i = 0; i < kMaxBoxes; i++) boxObj[i] = createReservedCube(scene, kBoxSize, kBoxSize, kBoxSize, boxMat[i]);
}

// Shared by rebuildLevelGeometry() and reserveWorstCaseLevelGeometry()
// below: counts how many cells of each kind this level's grid needs.
inline void countLevelCells(const GameState &gs, int &floorFlat, int &floorEdge, int &wallCells) {
    floorFlat = floorEdge = wallCells = 0;
    for (int r = 0; r < gs.rows; r++) {
        for (int c = 0; c < gs.cols; c++) {
            if (gs.cell[r][c] == CELL_WALL) wallCells++;
            else if (gs.cell[r][c] == CELL_FLOOR) {
                if (isIslandEdgeCell(gs, r, c)) floorEdge++;
                else floorFlat++;
            }
        }
    }
}

// Scans every level once at startup for its worst-case cell counts and
// reserve()s floorObj/wallObj to that ceiling up front, so a level with
// more walls/floor than any level played before it doesn't force a real
// operator-new call at a random, possibly-fragmented moment mid-play.
inline void reserveWorstCaseLevelGeometry(Object *floorObj, Object *wallObj) {
    int maxFlat = 0, maxEdge = 0, maxWalls = 0;
    GameState tmp;
    for (int i = 0; i < kLevelCount; i++) {
        loadLevel(i, tmp);
        int flat, edge, walls;
        countLevelCells(tmp, flat, edge, walls);
        maxFlat = std::max(maxFlat, flat);
        maxEdge = std::max(maxEdge, edge);
        maxWalls = std::max(maxWalls, walls);
    }
    floorObj->vertices.reserve((size_t)maxFlat * 4 + (size_t)maxEdge * 20);
    floorObj->triangles.reserve((size_t)maxFlat * 2 + (size_t)maxEdge * 10);
    wallObj->vertices.reserve((size_t)maxWalls * 20);
    wallObj->triangles.reserve((size_t)maxWalls * 10);
}

// Re-fills the floor/wall Objects with the current level's tiles. Wall
// cells get a bottomless box; floor cells get a flat top quad, except the
// exposed rim of a wall-less "floating island" level (isIslandEdgeCell,
// sokoban_game.hpp), which gets a bottomless box of its own so the island
// reads as a solid slab. Capacity was already reserved to the worst case
// across all levels by reserveWorstCaseLevelGeometry() at startup, so
// these reserve() calls are just documentation -- they no-op in practice.
inline void rebuildLevelGeometry(const GameState &gs, Material *floorMat, Material *goalMat, Material *wallMat,
                                  Object *floorObj, Object *wallObj) {
    int floorFlat, floorEdge, wallCells;
    countLevelCells(gs, floorFlat, floorEdge, wallCells);

    floorObj->vertices.clear();
    floorObj->triangles.clear();
    wallObj->vertices.clear();
    wallObj->triangles.clear();
    // Both boxes here and wall boxes below drop their -Y face (20 verts,
    // 10 tris, not 24/12): addBoxFaces(includeBottom=false).
    floorObj->vertices.reserve((size_t)floorFlat * 4 + (size_t)floorEdge * 20);
    floorObj->triangles.reserve((size_t)floorFlat * 2 + (size_t)floorEdge * 10);
    wallObj->vertices.reserve((size_t)wallCells * 20);
    wallObj->triangles.reserve((size_t)wallCells * 10);

    constexpr int32_t wallHalf = kTile / 2;
    constexpr int32_t floorHalf = (kTile - kFloorGap) / 2;

    // Wall cells are merged into maximal straight runs (one box per run,
    // not per cell): adjacent wall cells share a face nobody ever sees, so
    // per-cell boxes waste ~4x the triangles the same footprint needs (a
    // 9x9 room's 32-cell border: 320 vs. ~40 merged). This is what keeps
    // Scene's internal renderQueue (Scene.hpp, no public reserve(), grows
    // via plain push_back) from ever needing more capacity than level 1 --
    // wall-less but always loaded first -- already grows it to at startup.
    bool consumed[kMaxRows][kMaxCols] = {};
    for (int r = 0; r < gs.rows; r++) {
        for (int c = 0; c < gs.cols;) {
            if (gs.cell[r][c] != CELL_WALL) { c++; continue; }
            int run = 1;
            while (c + run < gs.cols && gs.cell[r][c + run] == CELL_WALL) run++;
            if (run >= 2) {
                int32_t x0 = cellWorldX(c, gs.cols) - wallHalf, x1 = cellWorldX(c + run - 1, gs.cols) + wallHalf;
                addBoxFaces(wallObj, (x0 + x1) / 2, kWallH / 2, cellWorldZ(r, gs.rows), (x1 - x0) / 2, kWallH / 2,
                            wallHalf, wallMat, /*includeBottom=*/false);
                for (int i = 0; i < run; i++) consumed[r][c + i] = true;
            }
            c += run;
        }
    }
    for (int c = 0; c < gs.cols; c++) {
        for (int r = 0; r < gs.rows;) {
            if (gs.cell[r][c] != CELL_WALL || consumed[r][c]) { r++; continue; }
            int run = 1;
            while (r + run < gs.rows && gs.cell[r + run][c] == CELL_WALL && !consumed[r + run][c]) run++;
            int32_t z0 = cellWorldZ(r, gs.rows) - wallHalf, z1 = cellWorldZ(r + run - 1, gs.rows) + wallHalf;
            addBoxFaces(wallObj, cellWorldX(c, gs.cols), kWallH / 2, (z0 + z1) / 2, wallHalf, kWallH / 2,
                        (z1 - z0) / 2, wallMat, /*includeBottom=*/false);
            for (int i = 0; i < run; i++) consumed[r + i][c] = true;
            r += run;
        }
    }

    for (int r = 0; r < gs.rows; r++) {
        for (int c = 0; c < gs.cols; c++) {
            if (gs.cell[r][c] != CELL_FLOOR) continue;
            int32_t x = cellWorldX(c, gs.cols);
            int32_t z = cellWorldZ(r, gs.rows);
            Material *mat = gs.goal[r][c] ? goalMat : floorMat;
            if (isIslandEdgeCell(gs, r, c)) {
                addBoxFaces(floorObj, x, -kIslandDepth / 2, z, floorHalf, kIslandDepth / 2, floorHalf, mat,
                            /*includeBottom=*/false);
            } else {
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

// How much board (radius, in tiles) stays in view around the player --
// fixed, not derived from the level's rows/cols, so the camera stays
// zoomed in on the cube the same amount on every level instead of pulling
// back for a bigger grid (see frameCameraOnPlayer() below).
constexpr float kFollowRadiusTiles = 3.0f;

// Places the camera on a fixed XZ-diagonal offset from (px, ?, pz) -- see
// main.cpp's top comment for why this fakes isometric -- then points it at
// that same spot via lookAt(). Called every frame with the player's
// current (possibly mid-slide) world position, so the camera pans to keep
// the cube centred and close instead of statically framing the whole
// board. `fovFactor` (cached on Camera by setFOV()) makes the
// apparent-size solve just apparentRadius = fovFactor*worldRadius/dist,
// inverted for dist; the offset itself doesn't depend on (px, pz), only
// on screen size, so most of this is loop-invariant work repeated for
// simplicity rather than because it needs to be.
inline void frameCameraOnPlayer(Camera &camera, int32_t px, int32_t pz, int width, int height) {
    camera.setFOV(50.0f, static_cast<int32_t>(width));

    float worldRadius = kFollowRadiusTiles * (float)kTile;

    // Leave room for the HUD strips (drawHud() below draws a 16px bar top
    // and bottom) so the view doesn't zoom in behind them.
    int usableHeight = height - 32;
    if (usableHeight < 1) usableHeight = height;
    float targetApparentRadius = 0.85f * (float)std::min(width, usableHeight);
    if (targetApparentRadius < 1.0f) targetApparentRadius = 1.0f;

    float dist = camera.fovFactor * worldRadius / targetApparentRadius;
    float s = dist / 1.7320508f; // camera offset (s,s,s) has length s*sqrt(3)
    int32_t offset = (int32_t)s;
    if (offset < kTile) offset = kTile; // guard a degenerate/zero FOV

    Vector3 target(px, kPlayerH / 2, pz);
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
