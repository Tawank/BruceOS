// sokoban_actors.hpp -- player/box Object creation and per-move visual
// updates for the Sokoban clone in main.cpp. Split out purely to keep
// sokoban_render.hpp under this app's 300-line-per-file budget; everything
// here builds on sokoban_render.hpp's constants and helpers.
#pragma once

#include "sokoban_render.hpp"

namespace {

// Platypus face: a wide, flat beak plus two eyes, both rebuilt (not just
// repositioned) whenever facing changes -- see rebuildFaceGeometry(). Y
// values are local to faceObj, which setPosition() places at the body's
// mid-height each frame (like playerObj), so 0 here means body-centre,
// negative means below it, positive above -- not floor-relative.
constexpr int32_t kBeakLen     = 12; // protrusion length, along travel
constexpr int32_t kBeakWidth   = 22; // width across the face
constexpr int32_t kBeakH       = 10; // flat vertical thickness
constexpr int32_t kBeakOffset  = kPlayerW / 2 + kBeakLen / 2;
constexpr int32_t kBeakY       = -kPlayerH / 8; // a bit below centre: mouth height
constexpr int32_t kEyeSize     = 6;
constexpr int32_t kEyeSpread   = 8; // +/- offset across the perpendicular axis
constexpr int32_t kEyeForward  = kPlayerW / 2 - 2;
constexpr int32_t kEyeY        = kPlayerH / 3; // above centre: near the top of the head

// Feet: fixed relative to the body (the camera never orbits, so a fixed
// pair reads fine regardless of facing) -- baked straight into the body
// mesh in createPlayerBody(), never rebuilt.
constexpr int32_t kFootSize   = 8;
constexpr int32_t kFootY      = -kPlayerH / 2 + 2;
constexpr int32_t kFootSpread = kPlayerW / 2 + kFootSize / 2 - 2;

// Triangle counts of the fixed-size compound meshes below (every sub-box
// is built with includeBottom=false, i.e. 20 verts/10 tris each, so verts
// are always exactly 2x tris). Named so worstCaseFrameTriangles() below
// doesn't duplicate these as raw numbers.
constexpr int kPlayerBodyTris = 30; // body + 2 feet
constexpr int kFaceTris       = 30; // beak + 2 eyes
constexpr int kCrateTris      = 24; // main box + 3 visible corner posts

// Body cube plus two small fixed feet poking out past its bottom corners,
// all reserve()d to their exact final size up front. bodyMat is
// direction-shaded (fake-sun, sokoban_geometry.hpp); footMat stays a
// single flat colour -- feet are too small for the shading to read.
inline Object *createPlayerBody(Scene &scene, const ShadedMaterial &bodyMat, Material *footMat) {
    Object *obj = new Object();
    obj->vertices.reserve(kPlayerBodyTris * 2);
    obj->triangles.reserve(kPlayerBodyTris);
    addBoxFaces(obj, 0, 0, 0, kPlayerW / 2, kPlayerH / 2, kPlayerW / 2, bodyMat.top, bodyMat.lit, bodyMat.shadow,
                /*includeBottom=*/false);
    constexpr int32_t fh = kFootSize / 2;
    addBoxFaces(obj, kFootSpread, kFootY, 0, fh, fh, fh, footMat, /*includeBottom=*/false);
    addBoxFaces(obj, -kFootSpread, kFootY, 0, fh, fh, fh, footMat, /*includeBottom=*/false);
    obj->calculateBoundingBox();
    scene.addObject(obj);
    return obj;
}

// One crate-textured box (addCrateFaces(), sokoban_geometry.hpp), reserve()d
// to its exact final size up front.
inline Object *createCrateObject(Scene &scene, const ShadedMaterial &mat, Material *edgeMat) {
    Object *obj = new Object();
    obj->vertices.reserve(kCrateTris * 2);
    obj->triangles.reserve(kCrateTris);
    addCrateFaces(obj, 0, 0, 0, kBoxSize / 2, mat, edgeMat);
    obj->calculateBoundingBox();
    scene.addObject(obj);
    return obj;
}

// Player body, its face (beak + eyes, filled in by rebuildFaceGeometry()
// below), and kMaxBoxes crates -- created once up front; levels with fewer
// boxes just disable the extra slots (see applyLevelToActors()) instead of
// creating/destroying Objects per level.
inline void createDynamicObjects(Scene &scene, const ShadedMaterial &playerMat, Material *accentMat,
                                  ShadedMaterial boxMat[], Object *&playerObj, Object *&faceObj, Object *boxObj[]) {
    playerObj = createPlayerBody(scene, playerMat, accentMat);
    faceObj = new Object();
    faceObj->vertices.reserve(kFaceTris * 2);
    faceObj->triangles.reserve(kFaceTris);
    scene.addObject(faceObj);
    for (int i = 0; i < kMaxBoxes; i++) boxObj[i] = createCrateObject(scene, boxMat[i], accentMat);
}

// Rebuilds faceObj's beak + eyes for the current facing direction -- unlike
// setPosition() (called every frame to track the player, body included),
// this is only called when facing actually changes. Beak/eyes swap their
// wide axis with facing, so the beak always reads as wide-across,
// thin-along-travel no matter which of the 4 axis-aligned directions the
// player is facing. beakMat is direction-shaded like the body (fake-sun,
// sokoban_geometry.hpp) -- it stays axis-aligned in world space regardless
// of facing, so the same top/lit/shadow tones apply correctly either way;
// eyeMat stays flat (too small for the shading to read).
inline void rebuildFaceGeometry(Object *faceObj, const ShadedMaterial &beakMat, Material *eyeMat, int facingDr,
                                 int facingDc) {
    faceObj->vertices.clear();
    faceObj->triangles.clear();
    faceObj->vertices.reserve(kFaceTris * 2);
    faceObj->triangles.reserve(kFaceTris);

    int32_t bx = facingDc * kBeakOffset, bz = facingDr * kBeakOffset;
    int32_t beakHw = facingDc != 0 ? kBeakLen / 2 : kBeakWidth / 2;
    int32_t beakHd = facingDc != 0 ? kBeakWidth / 2 : kBeakLen / 2;
    addBoxFaces(faceObj, bx, kBeakY, bz, beakHw, kBeakH / 2, beakHd, beakMat.top, beakMat.lit, beakMat.shadow,
                /*includeBottom=*/false);

    int32_t ex = facingDc * kEyeForward, ez = facingDr * kEyeForward;
    constexpr int32_t eh = kEyeSize / 2;
    if (facingDc != 0) {
        addBoxFaces(faceObj, ex, kEyeY, kEyeSpread, eh, eh, eh, eyeMat, /*includeBottom=*/false);
        addBoxFaces(faceObj, ex, kEyeY, -kEyeSpread, eh, eh, eh, eyeMat, /*includeBottom=*/false);
    } else {
        addBoxFaces(faceObj, kEyeSpread, kEyeY, ez, eh, eh, eh, eyeMat, /*includeBottom=*/false);
        addBoxFaces(faceObj, -kEyeSpread, kEyeY, ez, eh, eh, eh, eyeMat, /*includeBottom=*/false);
    }
    faceObj->calculateBoundingBox();
}

// Snaps player/face/box visuals straight to their logical grid cells (no
// animation); box slots beyond boxCount are disabled, not repositioned.
inline void applyLevelToActors(const GameState &gs, Object *playerObj, Object *faceObj, Object *boxObj[],
                                ShadedMaterial boxMat[], const ShadedMaterial &beakMat, Material *eyeMat,
                                int facingDr, int facingDc) {
    int32_t px = cellWorldX(gs.playerC, gs.cols);
    int32_t pz = cellWorldZ(gs.playerR, gs.rows);
    constexpr int32_t py = kPlayerH / 2;
    playerObj->setPosition(px, py, pz);
    rebuildFaceGeometry(faceObj, beakMat, eyeMat, facingDr, facingDc);
    faceObj->setPosition(px, py, pz);

    for (int i = 0; i < kMaxBoxes; i++) {
        if (i >= gs.boxCount) {
            boxObj[i]->enabled = false;
            continue;
        }
        boxObj[i]->enabled = true;
        bool onGoal = gs.goal[gs.boxR[i]][gs.boxC[i]];
        setShadedColor(boxMat[i], onGoal ? kBoxOnGoalColor : kBoxColor);
        int32_t bx = cellWorldX(gs.boxC[i], gs.cols);
        int32_t bz = cellWorldZ(gs.boxR[i], gs.rows);
        boxObj[i]->setPosition(bx, kBoxSize / 2, bz);
    }
}

} // namespace
