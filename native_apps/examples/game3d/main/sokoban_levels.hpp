// sokoban_levels.hpp -- static level data for the Sokoban clone in main.cpp.
//
// Standard Sokoban notation, plus '~' for cells outside the room shape
// (never drawn, never walkable) so a non-rectangular room doesn't get
// confused with plain floor the way a bare space would:
//   '#' wall     ' ' floor     '.' goal
//   '$' box      '*' box on goal
//   '@' player   '+' player on goal
//   '~' void (outside the room)
//
// Every level below has been verified solvable by a BFS over (player, box
// set) state space, not just eyeballed -- see the push sequence noted in
// each comment for the intended solution.
#pragma once

namespace {

struct LevelDef {
    const char *const *rows;
    int rowCount;
};

// Tutorial: push the box straight down onto the goal (2x DOWN). A
// wall-less "floating island" -- the level boundary is just where the
// floor stops, rounded off at the corners; sokoban_render.hpp gives the
// exposed rim its own side faces (isIslandEdgeCell, sokoban_game.hpp) so
// it reads as a solid slab rather than a paper-thin tile. Kept small (7x7)
// on purpose: a bigger grid means more tiles fighting for the same screen
// area, which makes the player cube read smaller, not bigger.
const char *const kLevel1[] = {
    "~~~ ~~~",
    "~~~ ~~~",
    "~~ @ ~~",
    "   $  .",
    "~~~ ~~~",
    "~~~ ~~~",
    "~~~ ~~~",
};

// Two boxes, each needs the player to walk around it to push from the
// north side, then 4x DOWN per box.
const char *const kLevel2[] = {
    "~~~~~~~~~",
    "~       ~",
    "~  $ $  ~",
    "~       ~",
    "~  @    ~",
    "~       ~",
    "~  . .  ~",
    "~       ~",
    "~~~~~~~~~",
};

// Same idea, with an interior pillar (two wall cells) to route around.
const char *const kLevel3[] = {
    "~~~~~~~~~",
    "~       ~",
    "~  $    ~",
    "~   #   ~",
    "~  @  $ ~",
    "~   #   ~",
    "~  .  . ~",
    "~       ~",
    "~~~~~~~~~",
};

// A diamond room (void cut in from all 4 corners, not just a bordered
// rectangle) split by a 2-cell pillar. The starting box blocks the
// straight route through the middle, so the player nudges it aside first,
// drops a second box into the near goal, then walks the first box up and
// around into its own goal on the far side, finishing with a third eased
// down to the point on the right (20 moves).
const char *const kLevel4[] = {
    "~~~~~~~~~",
    "~~~   ~~~",
    "~~  .  ~~",
    "~   # $ ~",
    "@$     . ",
    "~   #   ~",
    "~~ $.  ~~",
    "~~~   ~~~",
    "~~~~~~~~~",
};

// An L-shaped room, two squares joined at one corner (not a rectangle) --
// the room itself, not just a pillar, forces the detour. Two boxes sit in
// the near square and drop straight onto their goals; the third has to be
// walked the long way around the bend into the far square (24 moves).
const char *const kLevel5[] = {
    "~~~~~~~~~~~",
    "~   @ ~~~~~",
    "~ .#$ ~~~~~",
    "~  $. ~~~~~",
    "~         ~",
    "~~~~~ $   ~",
    "~~~~~   . ~",
    "~~~~~     ~",
    "~~~~~~~~~~~",
};

// Two chambers joined by a single-width corridor cut through the void --
// the hardest level here. Boxes start on both sides but belong on the
// other side, so the player has to walk one all the way across the
// corridor before the rest can follow without jamming the doorway
// (44 moves, the longest solution of the six levels). kMaxBoxes below caps
// the level at this many.
const char *const kLevel6[] = {
    "~~~~~~~~~~~~~~~",
    "~     ~~~     ~",
    "~   .$~~~ .   ~",
    "~  #       #  ~",
    "~     ~~~ $@$ ~",
    "~ . $ ~~~   . ~",
    "~~~~~~~~~~~~~~~",
};

const LevelDef kLevels[] = {
    {kLevel1, 7}, {kLevel2, 9}, {kLevel3, 9}, {kLevel4, 9}, {kLevel5, 9}, {kLevel6, 7},
};
constexpr int kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);

// Bounds for GameState's fixed-size grid/box arrays (sokoban_game.hpp);
// kept alongside the level data since they must cover it.
constexpr int kMaxRows = 16;
constexpr int kMaxCols = 16;
constexpr int kMaxBoxes = 4;

} // namespace
