// sokoban_geometry.hpp -- low-level box/quad mesh builders shared by
// sokoban_render.hpp. Split out purely to keep sokoban_render.hpp under
// this app's 300-line-per-file budget; these two functions have no
// dependency on GameState or Scene, just an Object to append into.
#pragma once

#include <cstdint>

#include "Jet.hpp" // IWYU pragma: keep

using namespace Renderer;

namespace {

// Appends one axis-aligned box's faces (per-face normals) to `obj`,
// centred at (cx, cy, cz) -- parameterised by a centre offset and pushed
// into a caller-owned Object instead of a fresh one, so many grid cells
// can share one Object (one culling test, one draw walk).
//
// `includeBottom` drops the -Y face (24 verts -> 20) when the caller
// knows it can never be seen -- worth doing given how memory-tight this
// loader's freestanding allocator is (see sokoban_render.hpp's
// createStaticGeometryObjects() comment).
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

// Appends a floating-island floor tile's top face plus a rim wall on ONLY
// the sides that actually face void/off-board (north/south/east/west,
// each true when that neighbour is exposed -- see isIslandEdgeCell() in
// sokoban_game.hpp). A tile bordering another floor tile on 2 or 3 of its
// sides doesn't get a wall face there: that neighbour is flat ground at
// the same y=0, so a full addBoxFaces() box hung an extra wall down past
// every interior-facing side too, reading as a raised block instead of a
// tile that blends into its neighbours. Worst case (an isolated 1-tile
// island, all 4 sides exposed) is still the 20 verts / 10 tris a full
// bottomless box needs, so callers can keep sizing reserve() the same way.
inline void addIslandRimFaces(Object *obj, int32_t cx, int32_t cz, int32_t half, int32_t depth, Material *mat,
                               bool north, bool south, bool east, bool west) {
    const int32_t N = FIXED_POINT_SCALE;
    const int32_t cy = -depth / 2, hh = depth / 2;
    uint16_t b;

    // Top (+Y) -- every rim tile shows its top.
    b = (uint16_t)obj->vertices.size();
    obj->addVertex({{cx - half, cy + hh, cz + half}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + half, cy + hh, cz + half}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + half, cy + hh, cz - half}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx - half, cy + hh, cz - half}, {0, 0}, {0, N, 0}});
    obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);

    // South (+Z)
    if (south) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx - half, cy - hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addVertex({{cx + half, cy - hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addVertex({{cx + half, cy + hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addVertex({{cx - half, cy + hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
    }
    // North (-Z)
    if (north) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx + half, cy - hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addVertex({{cx - half, cy - hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addVertex({{cx - half, cy + hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addVertex({{cx + half, cy + hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
    }
    // West (-X)
    if (west) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx - half, cy - hh, cz - half}, {0, 0}, {-N, 0, 0}});
        obj->addVertex({{cx - half, cy - hh, cz + half}, {0, 0}, {-N, 0, 0}});
        obj->addVertex({{cx - half, cy + hh, cz + half}, {0, 0}, {-N, 0, 0}});
        obj->addVertex({{cx - half, cy + hh, cz - half}, {0, 0}, {-N, 0, 0}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
    }
    // East (+X)
    if (east) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx + half, cy - hh, cz + half}, {0, 0}, {N, 0, 0}});
        obj->addVertex({{cx + half, cy - hh, cz - half}, {0, 0}, {N, 0, 0}});
        obj->addVertex({{cx + half, cy + hh, cz - half}, {0, 0}, {N, 0, 0}});
        obj->addVertex({{cx + half, cy + hh, cz + half}, {0, 0}, {N, 0, 0}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
    }
}

// Appends a single flat +Y-facing quad (4 verts) at height `cy`, centred
// at (cx, cz) -- the top face of addBoxFaces() above, standalone, for a
// floor tile whose sides are never visible (an interior tile with floor
// on every side).
inline void addTopQuad(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t hw, int32_t hd, Material *mat) {
    const int32_t N = FIXED_POINT_SCALE;
    uint16_t b = (uint16_t)obj->vertices.size();
    obj->addVertex({{cx - hw, cy, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy, cz - hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx - hw, cy, cz - hd}, {0, 0}, {0, N, 0}});
    obj->addFace(b + 0, b + 1, b + 2, b + 3, mat);
}

// Appends a cubic box plus thin dark beams along its 8 visible edges (4
// vertical corner posts, 4 around the top perimeter -- the 4 edges of the
// never-rendered bottom face are skipped). Each beam is centred ON its
// edge and poked out a little past the main box to both sides of it, so
// it reads as a highlighted border right where two faces actually meet.
// (An earlier version ran 3 straps through the box's CENTRE instead --
// each one only poked out at the middle of a face, nowhere near an edge,
// which just looked like a dark hole punched in the centre of that face.)
inline void addCrateFaces(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t half, Material *mat,
                           Material *edgeMat) {
    addBoxFaces(obj, cx, cy, cz, half, half, half, mat, /*includeBottom=*/false);
    constexpr int32_t e = 2; // edge beam half-thickness
    addBoxFaces(obj, cx + half, cy, cz + half, e, half, e, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx + half, cy, cz - half, e, half, e, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx - half, cy, cz + half, e, half, e, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx - half, cy, cz - half, e, half, e, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx, cy + half, cz + half, half, e, e, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx, cy + half, cz - half, half, e, e, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx + half, cy + half, cz, e, e, half, edgeMat, /*includeBottom=*/false);
    addBoxFaces(obj, cx - half, cy + half, cz, e, e, half, edgeMat, /*includeBottom=*/false);
}

} // namespace
