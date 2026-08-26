// sokoban_render.hpp -- Jet scene building, camera framing and HUD for the
// Sokoban clone in main.cpp. Free functions only (no namespace-scope
// instances -- see main.cpp's __dso_handle comment on this loader's ABI).
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

// Colours
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t kBackColor      = rgb565(16, 18, 28);
constexpr uint16_t kFloorColor     = rgb565(70, 76, 96);
constexpr uint16_t kGoalColor      = rgb565(150, 122, 40);
constexpr uint16_t kWallColor      = rgb565(58, 52, 66);
constexpr uint16_t kPlayerColor    = rgb565(60, 190, 225);
constexpr uint16_t kBeakColor      = rgb565(255, 200, 60);
constexpr uint16_t kAccentColor    = rgb565(18, 18, 22); // eyes, feet, crate straps
constexpr uint16_t kBoxColor       = rgb565(196, 122, 45);
constexpr uint16_t kBoxOnGoalColor = rgb565(80, 210, 100);
constexpr uint16_t kHudBg          = rgb565(8, 9, 14);
constexpr uint16_t kSolvedBg       = rgb565(18, 58, 22);

// Height (screen rows) of each HUD bar. main.cpp renders the 3D scene into
// a framebuffer this much shorter than the physical screen, top and
// bottom, so the bars live in their own rows the scene blit never touches.
constexpr int kHudBarH = 16;

// World-scale constants (same rough scale as the original spinning-cube
// demo's cubeSize -- see JetConfig.hpp).
constexpr int32_t kTile     = 40; // grid pitch
constexpr int32_t kFloorGap = 3;  // gap between floor tiles, purely cosmetic
constexpr int32_t kWallH    = 40;
constexpr int32_t kIslandDepth = 26; // side-face depth for a wall-less level's exposed rim
constexpr int32_t kBoxSize  = 34; // objects sized close to the tile on purpose -- see frameCameraForBoard()
constexpr int32_t kPlayerW  = 26;
constexpr int32_t kPlayerH  = 34;

inline int32_t cellWorldX(int c, int cols) { return (2 * c - (cols - 1)) * (kTile / 2); }
inline int32_t cellWorldZ(int r, int rows) { return (2 * r - (rows - 1)) * (kTile / 2); }

// Creates the floor/wall Objects ONCE for the app's whole lifetime; levels
// are re-rendered in place (rebuildLevelGeometry() below), never
// deleted/reallocated -- this loader's operator new returns NULL instead
// of throwing and nothing downstream checks that (elf_loader_sdk_symbols.c).
inline void createStaticGeometryObjects(Scene &scene, Object *&floorObj, Object *&wallObj) {
    floorObj = new Object();
    wallObj = new Object();
    scene.addObject(floorObj);
    scene.addObject(wallObj);
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

// Used by reserveWorstCaseLevelGeometry() below: the worst single-level
// cell count of each kind across all levels (not necessarily all from the
// same level -- floorObj and wallObj are two separate, independently-sized
// Objects, each needing its own all-time high-water mark, so cross-level
// maxima are what they actually need, unlike worstCaseFrameTriangles()
// in sokoban_actors.hpp, which sizes one shared per-frame budget and needs
// a real single level's total instead).
inline void worstCaseCellCounts(int &maxFlat, int &maxEdge, int &maxWalls) {
    maxFlat = maxEdge = maxWalls = 0;
    GameState tmp;
    for (int i = 0; i < kLevelCount; i++) {
        loadLevel(i, tmp);
        int flat, edge, walls;
        countLevelCells(tmp, flat, edge, walls);
        maxFlat = std::max(maxFlat, flat);
        maxEdge = std::max(maxEdge, edge);
        maxWalls = std::max(maxWalls, walls);
    }
}

// Reserve()s floorObj/wallObj to the worst-case ceiling above, so a bigger
// later level never forces a real operator-new at a random, fragmented
// moment mid-play.
inline void reserveWorstCaseLevelGeometry(Object *floorObj, Object *wallObj) {
    int maxFlat, maxEdge, maxWalls;
    worstCaseCellCounts(maxFlat, maxEdge, maxWalls);
    floorObj->vertices.reserve((size_t)maxFlat * 4 + (size_t)maxEdge * 20);
    floorObj->triangles.reserve((size_t)maxFlat * 2 + (size_t)maxEdge * 10);
    wallObj->vertices.reserve((size_t)maxWalls * 20);
    wallObj->triangles.reserve((size_t)maxWalls * 10);
}

// Re-fills the floor/wall Objects with the current level's tiles. Floor
// cells get a flat top quad, except a wall-less "island" level's exposed
// rim (isIslandEdgeCell, sokoban_game.hpp), which gets a bottomless box so
// it reads as a solid slab. Capacity was already reserved to the worst
// case at startup, so the reserve() calls below just document intent.
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

    // Wall cells are merged into maximal straight runs (one box per run, not
    // per cell): adjacent cells share a face nobody ever sees, so per-cell
    // boxes waste ~4x the triangles the same footprint needs (a 9x9 room's
    // 32-cell border: 320 vs. ~40 merged) -- keeping every level's geometry
    // under level 1's (wall-less, always loaded first) startup triangle count.
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

// Fixed board radius (tiles) kept in view around the player -- not
// derived from rows/cols, so zoom stays constant across levels instead of
// pulling back for a bigger grid (frameCameraOnPlayer() below).
constexpr float kFollowRadiusTiles = 3.0f;

// Places the camera on a fixed XZ-diagonal offset from (px, ?, pz) -- see
// main.cpp's top comment for why this fakes isometric -- then lookAt()s
// that spot. Called every frame with the player's current (possibly
// mid-slide) position, so the camera pans to keep the cube centred.
// `fovFactor` (cached by setFOV()) makes apparentRadius =
// fovFactor*worldRadius/dist solvable for dist.
inline void frameCameraOnPlayer(Camera &camera, int32_t px, int32_t pz, int width, int height) {
    camera.setFOV(50.0f, static_cast<int32_t>(width));

    float worldRadius = kFollowRadiusTiles * (float)kTile;
    // `height` here is already the game viewport (main.cpp reserves the HUD
    // bars out of the framebuffer itself), so no separate margin is needed.
    float targetApparentRadius = 0.85f * (float)std::min(width, height);
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

// Only touches the display when the HUD's content changed: the bars now
// live in their own screen rows (kHudBarH above), untouched by the scene
// blit. Text uses an opaque, fixed-width background instead of a separate
// fill_rect erase -- redoing that erase+redraw every dirty frame (~6x per
// move, during slide animation) was what caused the visible flicker.
inline void drawHud(const GameState &gs, int levelIndex, int width, int height) {
    static int lastLevel = -1, lastMoves = -1;
    static bool lastSolved = false, everDrawn = false;
    bool firstDraw = !everDrawn;
    bool bannerChanged = firstDraw || gs.solved != lastSolved;
    if (!firstDraw && !bannerChanged && levelIndex == lastLevel && gs.moves == lastMoves) return;
    lastLevel = levelIndex;
    lastMoves = gs.moves;
    lastSolved = gs.solved;
    everDrawn = true;

    if (firstDraw) display__fill_rect(0, 0, (int16_t)width, (int16_t)kHudBarH, kHudBg);
    char line[48];
    std::snprintf(line, sizeof(line), "Level %d/%d   Moves %-5d", levelIndex + 1, kLevelCount, gs.moves);
    display__set_text_color(BRUCE_COLOR_WHITE);
    display__set_text_bg_color(kHudBg);
    display__draw_string(line, 4, 4);

    if (!bannerChanged) return;
    int16_t by = (int16_t)(height - kHudBarH);
    if (gs.solved) {
        display__fill_rect(0, by, (int16_t)width, (int16_t)kHudBarH, kSolvedBg);
        display__set_text_color(rgb565(255, 235, 130));
        display__set_text_bg_color(kSolvedBg);
        display__draw_centre_string("SOLVED! SELECT for next level", (int16_t)(width / 2), (int16_t)(by + 4));
    } else {
        display__fill_rect(0, by, (int16_t)width, (int16_t)kHudBarH, kHudBg);
        display__set_text_color(rgb565(150, 158, 180));
        display__set_text_bg_color(kHudBg);
        display__draw_centre_string("SELECT restart  BACK exit", (int16_t)(width / 2), (int16_t)(by + 4));
    }
}

} // namespace
