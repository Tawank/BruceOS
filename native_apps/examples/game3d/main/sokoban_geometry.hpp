// sokoban_geometry.hpp -- low-level box/quad mesh builders shared by
// sokoban_render.hpp. Split out purely to keep sokoban_render.hpp under
// this app's 300-line-per-file budget; these two functions have no
// dependency on GameState or Scene, just an Object to append into.
#pragma once

#include <cstdint>

#include "Jet.hpp" // IWYU pragma: keep

using namespace Renderer;

namespace {

// Fake-sun face shading. Every axis-aligned block this app builds (walls,
// crates, the player body, island floor rims) only ever shows 3 of its 6
// faces to the fixed, non-orbiting isometric camera (frameCameraOnPlayer(),
// sokoban_render.hpp): the +Y top, +X "right" and -Z "back" faces -- the
// other three are always backface-culled (Scene.cpp's CULL_BACKFACES).
// Rather than a real lighting pass (LIGHTING is off in JetConfig.hpp on
// purpose: extra bytes per vertex and a Lambert term every frame this
// scene never needs), each face just gets one of 3 Materials precomputed
// once from a single base colour by scaling its RGB565 channels -- imagine
// the sun high overhead and toward +X/-Z: top stays full brightness, +X
// gets a partial shade, -Z sits in shadow. addBoxFaces()/addIslandRimFaces()
// below hand the darker two to whichever face never actually draws too,
// but that costs nothing -- only the three visible tones affect the look.
constexpr int kShadeTop    = 100; // top face: unmodified base colour
constexpr int kShadeLit    = 78;  // +X face: partial shade
constexpr int kShadeShadow = 50;  // -Z face: full shadow

constexpr uint16_t shadeColor(uint16_t color, int percent) {
    int r = ((color >> 11) & 0x1F) * percent / 100;
    int g = ((color >> 5) & 0x3F) * percent / 100;
    int b = (color & 0x1F) * percent / 100;
    if (r > 0x1F) r = 0x1F;
    if (g > 0x3F) g = 0x3F;
    if (b > 0x1F) b = 0x1F;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// One block colour's 3 precomputed face Materials (see shadeColor() above).
// Deliberately a plain aggregate, not a class with constructors/destructors
// -- this loader's ABI forbids namespace-scope instances with non-trivial
// initialisation (main.cpp's __dso_handle comment), so every ShadedMaterial
// lives as a local in main.cpp, explicitly built/torn down by the
// make/delete helpers below exactly like every other Material* in this app.
struct ShadedMaterial {
    Material *top;
    Material *lit;
    Material *shadow;
};

// operator new returns NULL on failure here instead of throwing (loader
// convention, see main.cpp) -- returns false if any of the 3 allocations
// failed so callers can bail out the same way they already do for a plain
// Material*.
inline bool makeShadedMaterial(ShadedMaterial &m, uint16_t baseColor) {
    m.top = new Material(shadeColor(baseColor, kShadeTop));
    m.lit = new Material(shadeColor(baseColor, kShadeLit));
    m.shadow = new Material(shadeColor(baseColor, kShadeShadow));
    return m.top != nullptr && m.lit != nullptr && m.shadow != nullptr;
}

// Re-derives all 3 tones from a new base colour in place -- used instead of
// allocating fresh Materials when a box's colour changes at runtime (landing
// on/off a goal tile), same spirit as the plain `mat->color = ...` swap this
// replaced.
inline void setShadedColor(ShadedMaterial &m, uint16_t baseColor) {
    m.top->color = shadeColor(baseColor, kShadeTop);
    m.lit->color = shadeColor(baseColor, kShadeLit);
    m.shadow->color = shadeColor(baseColor, kShadeShadow);
}

inline void deleteShadedMaterial(const ShadedMaterial &m) {
    delete m.top;
    delete m.lit;
    delete m.shadow;
}

// Appends one axis-aligned box's faces (per-face normals) to `obj`,
// centred at (cx, cy, cz) -- parameterised by a centre offset and pushed
// into a caller-owned Object instead of a fresh one, so many grid cells
// can share one Object (one culling test, one draw walk).
//
// `topMat`/`xMat`/`zMat` shade the box per the fake-sun scheme above: `xMat`
// covers both X faces (only +X, "right", is ever visible), `zMat` covers
// both Z faces (only -Z, "back", is ever visible), `topMat` covers +Y (and
// -Y, never visible) -- pass the same Material 3 times via the single-`mat`
// overload below for parts that don't need directional shading.
//
// `includeBottom` drops the -Y face (24 verts -> 20) when the caller
// knows it can never be seen -- worth doing given how memory-tight this
// loader's freestanding allocator is (see sokoban_render.hpp's
// createStaticGeometryObjects() comment).
inline void addBoxFaces(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t hw, int32_t hh, int32_t hd,
                         Material *topMat, Material *xMat, Material *zMat, bool includeBottom = true) {
    const int32_t N = FIXED_POINT_SCALE;
    uint16_t b = (uint16_t)obj->vertices.size();

    // Front (+Z) -- shares zMat with the back face below; never actually
    // drawn from this app's fixed camera (backface-culled), so which
    // z-axis tone it gets makes no visual difference.
    obj->addVertex({{cx - hw, cy - hh, cz + hd}, {0, 0}, {0, 0, N}});
    obj->addVertex({{cx + hw, cy - hh, cz + hd}, {0, 0}, {0, 0, N}});
    obj->addVertex({{cx + hw, cy + hh, cz + hd}, {0, 0}, {0, 0, N}});
    obj->addVertex({{cx - hw, cy + hh, cz + hd}, {0, 0}, {0, 0, N}});
    // Back (-Z) -- the z-axis face the camera actually sees: the shadow
    // side of the fake sun.
    obj->addVertex({{cx + hw, cy - hh, cz - hd}, {0, 0}, {0, 0, -N}});
    obj->addVertex({{cx - hw, cy - hh, cz - hd}, {0, 0}, {0, 0, -N}});
    obj->addVertex({{cx - hw, cy + hh, cz - hd}, {0, 0}, {0, 0, -N}});
    obj->addVertex({{cx + hw, cy + hh, cz - hd}, {0, 0}, {0, 0, -N}});
    // Left (-X) -- shares xMat with the right face; never actually drawn.
    obj->addVertex({{cx - hw, cy - hh, cz - hd}, {0, 0}, {-N, 0, 0}});
    obj->addVertex({{cx - hw, cy - hh, cz + hd}, {0, 0}, {-N, 0, 0}});
    obj->addVertex({{cx - hw, cy + hh, cz + hd}, {0, 0}, {-N, 0, 0}});
    obj->addVertex({{cx - hw, cy + hh, cz - hd}, {0, 0}, {-N, 0, 0}});
    // Right (+X) -- the x-axis face the camera actually sees: the lit
    // side of the fake sun.
    obj->addVertex({{cx + hw, cy - hh, cz + hd}, {0, 0}, {N, 0, 0}});
    obj->addVertex({{cx + hw, cy - hh, cz - hd}, {0, 0}, {N, 0, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz - hd}, {0, 0}, {N, 0, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz + hd}, {0, 0}, {N, 0, 0}});
    // Top (+Y) -- always visible, brightest: the sun shines straight down.
    obj->addVertex({{cx - hw, cy + hh, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz + hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + hw, cy + hh, cz - hd}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx - hw, cy + hh, cz - hd}, {0, 0}, {0, N, 0}});

    obj->addFace(b + 0, b + 1, b + 2, b + 3, zMat);
    obj->addFace(b + 4, b + 5, b + 6, b + 7, zMat);
    obj->addFace(b + 8, b + 9, b + 10, b + 11, xMat);
    obj->addFace(b + 12, b + 13, b + 14, b + 15, xMat);
    obj->addFace(b + 16, b + 17, b + 18, b + 19, topMat);

    if (!includeBottom) return;
    // Bottom (-Y) -- never visible; reuses topMat rather than pay for a
    // 4th Material nobody will ever see the effect of.
    obj->addVertex({{cx - hw, cy - hh, cz - hd}, {0, 0}, {0, -N, 0}});
    obj->addVertex({{cx + hw, cy - hh, cz - hd}, {0, 0}, {0, -N, 0}});
    obj->addVertex({{cx + hw, cy - hh, cz + hd}, {0, 0}, {0, -N, 0}});
    obj->addVertex({{cx - hw, cy - hh, cz + hd}, {0, 0}, {0, -N, 0}});
    obj->addFace(b + 20, b + 21, b + 22, b + 23, topMat);
}

// Convenience overload for parts that don't need directional shading (small
// trim: player feet, eyes, beak) -- every face gets the same flat colour,
// same as before the fake-sun shading above was added.
inline void addBoxFaces(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t hw, int32_t hh, int32_t hd,
                         Material *mat, bool includeBottom = true) {
    addBoxFaces(obj, cx, cy, cz, hw, hh, hd, mat, mat, mat, includeBottom);
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
//
// `topMat`/`litMat`/`shadowMat` follow the same fake-sun scheme as
// addBoxFaces() above: east (+X) is the lit side, north (-Z) is the shadow
// side; south/west never face the camera so they just reuse whichever tone
// their axis carries.
inline void addIslandRimFaces(Object *obj, int32_t cx, int32_t cz, int32_t half, int32_t depth, Material *topMat,
                               Material *litMat, Material *shadowMat, bool north, bool south, bool east, bool west) {
    const int32_t N = FIXED_POINT_SCALE;
    const int32_t cy = -depth / 2, hh = depth / 2;
    uint16_t b;

    // Top (+Y) -- every rim tile shows its top.
    b = (uint16_t)obj->vertices.size();
    obj->addVertex({{cx - half, cy + hh, cz + half}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + half, cy + hh, cz + half}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx + half, cy + hh, cz - half}, {0, 0}, {0, N, 0}});
    obj->addVertex({{cx - half, cy + hh, cz - half}, {0, 0}, {0, N, 0}});
    obj->addFace(b + 0, b + 1, b + 2, b + 3, topMat);

    // South (+Z) -- shares shadowMat with north; never actually visible.
    if (south) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx - half, cy - hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addVertex({{cx + half, cy - hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addVertex({{cx + half, cy + hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addVertex({{cx - half, cy + hh, cz + half}, {0, 0}, {0, 0, N}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, shadowMat);
    }
    // North (-Z) -- the rim face the camera actually sees: shadow side.
    if (north) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx + half, cy - hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addVertex({{cx - half, cy - hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addVertex({{cx - half, cy + hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addVertex({{cx + half, cy + hh, cz - half}, {0, 0}, {0, 0, -N}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, shadowMat);
    }
    // West (-X) -- shares litMat with east; never actually visible.
    if (west) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx - half, cy - hh, cz - half}, {0, 0}, {-N, 0, 0}});
        obj->addVertex({{cx - half, cy - hh, cz + half}, {0, 0}, {-N, 0, 0}});
        obj->addVertex({{cx - half, cy + hh, cz + half}, {0, 0}, {-N, 0, 0}});
        obj->addVertex({{cx - half, cy + hh, cz - half}, {0, 0}, {-N, 0, 0}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, litMat);
    }
    // East (+X) -- the rim face the camera actually sees: lit side.
    if (east) {
        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{cx + half, cy - hh, cz + half}, {0, 0}, {N, 0, 0}});
        obj->addVertex({{cx + half, cy - hh, cz - half}, {0, 0}, {N, 0, 0}});
        obj->addVertex({{cx + half, cy + hh, cz - half}, {0, 0}, {N, 0, 0}});
        obj->addVertex({{cx + half, cy + hh, cz + half}, {0, 0}, {N, 0, 0}});
        obj->addFace(b + 0, b + 1, b + 2, b + 3, litMat);
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

// Appends a cubic box plus the three vertical corner posts visible from the
// game's fixed (+X, -Z) camera. Each post contains only its camera-facing
// side(s) and top cap; the hidden faces and fourth rear corner are omitted.
// This keeps a clear crate silhouette without paying for complete little
// boxes that can never be seen. `mat` shades the main box per the fake-sun
// scheme above; `edgeMat` (the crate's dark corner/strap trim) stays a
// single flat colour on every post face -- it reads as a distinct trim
// accent, not part of the box's own diffuse shading.
inline void addCrateFaces(Object *obj, int32_t cx, int32_t cy, int32_t cz, int32_t half, const ShadedMaterial &mat,
                           Material *edgeMat) {
    addBoxFaces(obj, cx, cy, cz, half, half, half, mat.top, mat.lit, mat.shadow, /*includeBottom=*/false);

    constexpr int32_t e = 2;
    constexpr int32_t N = FIXED_POINT_SCALE;
    auto addPost = [&](int sx, int sz, bool showX, bool showZ) {
        const int32_t x = cx + sx * half;
        const int32_t z = cz + sz * half;
        const int32_t y0 = cy - half;
        const int32_t y1 = cy + half + 1;
        uint16_t b;

        if (showX) {
            b = (uint16_t)obj->vertices.size();
            const int32_t fx = x + sx;
            obj->addVertex({{fx, y0, z + sx * e}, {0, 0}, {sx * N, 0, 0}});
            obj->addVertex({{fx, y0, z - sx * e}, {0, 0}, {sx * N, 0, 0}});
            obj->addVertex({{fx, y1, z - sx * e}, {0, 0}, {sx * N, 0, 0}});
            obj->addVertex({{fx, y1, z + sx * e}, {0, 0}, {sx * N, 0, 0}});
            obj->addFace(b, b + 1, b + 2, b + 3, edgeMat);
        }
        if (showZ) {
            b = (uint16_t)obj->vertices.size();
            const int32_t fz = z + sz;
            obj->addVertex({{x + e, y0, fz}, {0, 0}, {0, 0, sz * N}});
            obj->addVertex({{x - e, y0, fz}, {0, 0}, {0, 0, sz * N}});
            obj->addVertex({{x - e, y1, fz}, {0, 0}, {0, 0, sz * N}});
            obj->addVertex({{x + e, y1, fz}, {0, 0}, {0, 0, sz * N}});
            obj->addFace(b, b + 1, b + 2, b + 3, edgeMat);
        }

        b = (uint16_t)obj->vertices.size();
        obj->addVertex({{x - e, y1, z + e}, {0, 0}, {0, N, 0}});
        obj->addVertex({{x + e, y1, z + e}, {0, 0}, {0, N, 0}});
        obj->addVertex({{x + e, y1, z - e}, {0, 0}, {0, N, 0}});
        obj->addVertex({{x - e, y1, z - e}, {0, 0}, {0, N, 0}});
        obj->addFace(b, b + 1, b + 2, b + 3, edgeMat);
    };

    addPost(+1, -1, true, true);  // near corner shared by both visible sides
    addPost(+1, +1, true, false); // far edge of the +X side
    addPost(-1, -1, false, true); // far edge of the -Z side
}

} // namespace
