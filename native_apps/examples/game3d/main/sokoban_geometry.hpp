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

} // namespace
